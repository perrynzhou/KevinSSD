#include "lsmtree.h"
#include "compaction.h"
#include "skiplist.h"
#include "page.h"
#include "bloomfilter.h"
#include "nocpy.h"
#include "lsmtree_scheduling.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sched.h>

#include "../../interface/interface.h"
#include "../../include/types.h"
#include "../../include/data_struct/list.h"
#include "../../include/utils/kvssd.h"
#ifdef DEBUG
#endif

#ifdef KVSSD
extern KEYT key_min, key_max;
#endif
/*
#define free(a) \
	do{\
		fprintf(stderr,"%s %d:%p\n",__FILE__,__LINE__,a);\
		free(a);\
	}while(0)
*/


volatile int memcpy_cnt;

extern lsmtree LSM;
extern block bl[_NOB];
extern volatile int comp_target_get_cnt;
volatile int epc_check=0;
int upper_table=0;
bool debug_condition;
compM compactor;
pthread_mutex_t compaction_wait;
pthread_mutex_t compaction_flush_wait;
pthread_mutex_t compaction_req_lock;
pthread_cond_t compaction_req_cond;
volatile int compaction_req_cnt;
bool compaction_idle;
volatile int compactino_target_cnt;
#if (LEVELN==1)
void onelevel_processing(run_t *);
#endif
static int compaction_cnt=0;
void compaction_sub_pre(){
	pthread_mutex_lock(&compaction_wait);
}

static void compaction_selector(level *a, level *b,leveling_node *lnode, pthread_mutex_t* lock){
	compaction_cnt++;
	MS(&LSM.timers[4]);
#if LEVELN==1
	level_one_processing(a,b,r,lock);
	return;
#endif
	if(b->istier){
		//tiering(a,b,r,lock);
	}
	else{
		leveling(a,b,lnode,lock);
	}
	MA(&LSM.timers[4]);
}

void compaction_sub_wait(){
#ifdef MUTEXLOCK
	if(epc_check==comp_target_get_cnt+memcpy_cnt)
		pthread_mutex_unlock(&compaction_wait);
#elif defined (SPINLOCK)
	while(comp_target_get_cnt+memcpy_cnt!=epc_check){}
#endif
	//printf("%u:%u\n",comp_target_get_cnt,epc_check);

	memcpy_cnt=0;
	comp_target_get_cnt=0;
	epc_check=0;
}

void compaction_sub_post(){
	pthread_mutex_unlock(&compaction_wait);
}

void htable_checker(htable *table){
	for(uint32_t i=0; i<LSM.KEYNUM; i++){
		if(table->sets[i].ppa<512 && table->sets[i].ppa!=0){
			printf("here!!\n");
		}
	}
}

void compaction_heap_setting(level *a, level* b){
#ifdef LEVELUSINGHEAP
	heap_free(a->h);
#else
	llog_free(a->h);
#endif
	a->h=b->h;
	a->now_block=b->now_block;
	b->h=NULL;
}

bool compaction_init(){
	compactor.processors=(compP*)malloc(sizeof(compP)*CTHREAD);
	memset(compactor.processors,0,sizeof(compP)*CTHREAD);

	pthread_mutex_init(&compaction_req_lock,NULL);
	pthread_cond_init(&compaction_req_cond,NULL);

	for(int i=0; i<CTHREAD; i++){
		compactor.processors[i].master=&compactor;
		pthread_mutex_init(&compactor.processors[i].flag, NULL);
		pthread_mutex_lock(&compactor.processors[i].flag);
		q_init(&compactor.processors[i].q,CQSIZE);
		pthread_create(&compactor.processors[i].t_id,NULL,compaction_main,NULL);
	}
	compactor.stopflag=false;
	pthread_mutex_init(&compaction_wait,NULL);
	pthread_mutex_init(&compaction_flush_wait,NULL);
	pthread_mutex_lock(&compaction_flush_wait);
	
#ifdef MERGECOMPACTION
	merge_compaction_init();
#endif

#ifdef WRITEOPTIMIZE
	lsm_io_sched_init();
#endif

	return true;
}


void compaction_free(){
	compactor.stopflag=true;
	int *temp;
	for(int i=0; i<CTHREAD; i++){
		compP *t=&compactor.processors[i];
		pthread_cond_signal(&compaction_req_cond);
		//pthread_mutex_unlock(&compaction_assign_lock);
		while(pthread_tryjoin_np(t->t_id,(void**)&temp)){
			pthread_cond_signal(&compaction_req_cond);
		}
		q_free(t->q);
	}
	printf("compaction_cnt:%d\n",compaction_cnt);
	free(compactor.processors);
}

void compaction_wait_done(){
	bool flag=false;
	while(1){
#ifdef LEAKCHECK
		sleep(2);
#endif
		for(int i=0; i<CTHREAD; i++){
			compP* proc=&compactor.processors[i];
			if(proc->q->size!=CQSIZE){
				flag=true;
				break;
			}
		}
		if(flag) break;
	}
}

void compaction_assign(compR* req){
	static int seq_num=0;
	bool flag=false;
	while(1){
#ifdef LEAKCHECK
		sleep(2);
#endif
		for(int i=0; i<CTHREAD; i++){
			compP* proc=&compactor.processors[i];
			req->seq=seq_num++;

			pthread_mutex_lock(&compaction_req_lock);
			if(proc->q->size==0){
				if(q_enqueue((void*)req,proc->q)){
					flag=true;
				}
				else{
					DEBUG_LOG("FUCK!");
				}
			}
			else {
				if(q_enqueue((void*)req,proc->q)){	
					flag=true;
				}
				else{
					flag=false;
				}
			}
			if(flag){
				pthread_cond_signal(&compaction_req_cond);
				pthread_mutex_unlock(&compaction_req_lock);
				break;
			}
			else{
				pthread_mutex_unlock(&compaction_req_lock);
			}
		}
		if(flag) break;
	}
}

extern master_processor mp;
bool isflushing;
void compaction_data_write(leveling_node* lnode){
	isflushing=true;
	//run_t *res=LSM.lop->make_run(key_max,key_min,-1);
	value_set **data_sets=skiplist_make_valueset(lnode->mem,LSM.disk[0],&lnode->start,&lnode->end);

#ifdef WRITEOPTIMIZE
	lsm_io_sched_push(SCHED_FLUSH,(void*)data_sets);//make flush background job
#else
	for(int i=0; data_sets[i]!=NULL; i++){	
		algo_req *lsm_req=(algo_req*)malloc(sizeof(algo_req));
		lsm_params *params=(lsm_params*)malloc(sizeof(lsm_params));
		params->lsm_type=DATAW;
		params->value=data_sets[i];
		lsm_req->parents=NULL;
		lsm_req->params=(void*)params;
		lsm_req->end_req=lsm_end_req;
		lsm_req->rapid=true;
		lsm_req->type=DATAW;
		if(params->value->dmatag==-1){
			abort();
		}
		LSM.li->write(data_sets[i]->ppa,PAGESIZE,params->value,ASYNC,lsm_req);
	}
	free(data_sets);
#endif
	//LSM.lop->mem_cvt2table(mem,res); //res's filter and table will be set
	isflushing=false;
}

uint32_t compaction_htable_write(htable *input, KEYT lpa, char *nocpy_data){
#ifdef KVSSD
	uint32_t ppa=getPPA(HEADER,key_min,true);
#else
	uint32_t ppa=getPPA(HEADER,0,true);//set ppa;
#endif
	algo_req *areq=(algo_req*)malloc(sizeof(algo_req));
	lsm_params *params=(lsm_params*)malloc(sizeof(lsm_params));
	areq->parents=NULL;
	areq->rapid=false;
	params->lsm_type=HEADERW;
	if(input->origin){
		printf("can't be - %s:%d\n",__FILE__,__LINE__);
		params->value=input->origin;
	}else{
		//this logic should process when the memtable's header write
		params->value=inf_get_valueset(NULL,FS_MALLOC_W,PAGESIZE);
	}

#ifdef NOCPY
	nocpy_copy_from_change((char*)nocpy_data,ppa);
	input->sets=NULL;
#endif
	params->htable_ptr=input;
	areq->end_req=lsm_end_req;
	areq->params=(void*)params;
	areq->type=HEADERW;
	params->ppa=ppa;
	
	LSM.li->write(ppa,PAGESIZE,params->value,ASYNC,areq);
	//printf("%u\n",ppa);
	return ppa;
}

void dummy_meta_write(uint32_t ppa){
	value_set *temp=inf_get_valueset(NULL,FS_MALLOC_W,PAGESIZE);
	algo_req *areq=(algo_req*)malloc(sizeof(algo_req));

	lsm_params *params=(lsm_params*)malloc(sizeof(lsm_params));
	areq->parents=NULL;
	areq->rapid=false;
	params->lsm_type=HEADERW;
	params->value=temp;
	params->htable_ptr=NULL;
	
	areq->end_req=lsm_end_req;
	areq->params=(void*)params;
	areq->type=HEADERW;
	params->ppa=ppa;
	LSM.li->write(ppa,PAGESIZE,params->value,ASYNC,areq);
}

bool compaction_force(){

	for(int i=LEVELN-2; i>=0; i--){
		if(LSM.disk[i]->n_num){
			compaction_selector(LSM.disk[i],LSM.disk[LEVELN-1],NULL,&LSM.level_lock[LEVELN-1]);
	//		printf("\n\n");
	//		level_summary();
			return true;
		}
	}/*
	printf("\n false after \n");
	level_summary();*/
	return false;
}
bool compaction_force_target(int from, int to){
	if(!LSM.disk[from]->n_num) return false;
	compaction_selector(LSM.disk[from],LSM.disk[to],NULL,&LSM.level_lock[to]);
	return true;
}

bool compaction_force_levels(int nol){
	for(int i=0; i<nol; i++){
		int max=0,target=0;
		for(int j=0; j<LEVELN-1; j++){
			int noe;
#ifdef LEVELCACHING
			if(j<LEVELCACHING){
				noe=LSM.lop->cache_get_size(LSM.disk[j]);
			}else{
#endif		
				noe=LSM.disk[j]->n_num;	
#ifdef LEVELCACHING
			}
#endif
			if(max<noe){
				max=noe;
				target=j;
			}
		}
		if(max!=0){
			compaction_selector(LSM.disk[target],LSM.disk[LEVELN-1],NULL,&LSM.level_lock[LEVELN-1]);
		}else{
			return false;
		}
	}
	return true;
}

extern pm data_m;
void *compaction_main(void *input){
	void *_req;
	compR*req;
	compP *_this=NULL;
	//static int ccnt=0;
	char thread_name[128]={0};
	pthread_t current_thread=pthread_self();
	sprintf(thread_name,"%s","compaction_thread");
	pthread_setname_np(pthread_self(),thread_name);

	for(int i=0; i<CTHREAD; i++){
		if(pthread_self()==compactor.processors[i].t_id){
			_this=&compactor.processors[i];
		}
	}
	while(1){
#ifdef LEAKCHECK
		sleep(2);
#endif
		pthread_mutex_lock(&compaction_req_lock);
		if(_this->q->size==0){
			pthread_cond_wait(&compaction_req_cond,&compaction_req_lock);
		}
		_req=q_pick(_this->q);
		pthread_mutex_unlock(&compaction_req_lock);
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(1,&cpuset);
		pthread_setaffinity_np(current_thread,sizeof(cpu_set_t),&cpuset);

		if(compactor.stopflag)
			break;


		int start_level=0,des_level;
		req=(compR*)_req;
		leveling_node lnode;
		if(req->fromL==-1){
			while(!gc_check(DATA,false)){
			}
			lnode.mem=LSM.temptable;
			compaction_data_write(&lnode);
			/*	pthread_mutex_lock(&LSM.templock);

			run_t *entry=compaction_data_write(LSM.temptable);
			pthread_mutex_unlock(&LSM.templock);
			pthread_mutex_lock(&LSM.entrylock);
			LSM.tempent=entry;
			pthread_mutex_unlock(&LSM.entrylock);*/

			compaction_selector(NULL,LSM.disk[0],&lnode,&LSM.level_lock[0]);
		}
#if LEVELN!=1
		while(1){
			if(unlikely(LSM.lop->full_check(LSM.disk[start_level]))){
				des_level=(start_level==LEVELN?start_level:start_level+1);
				if(des_level==LEVELN) break;
				compaction_selector(LSM.disk[start_level],LSM.disk[des_level],NULL,&LSM.level_lock[des_level]);
				LSM.disk[start_level]->iscompactioning=false;
				start_level++;
			}
			else{
				break;
			}
		}
#endif
		free(lnode.start.key);
		free(lnode.end.key);

		pthread_mutex_lock(&LSM.templock);
		skiplist_free(LSM.temptable);
		LSM.temptable=NULL;
		pthread_mutex_unlock(&LSM.templock);

		free(req);
#ifdef WRITEWAIT
		LSM.li->lower_flying_req_wait();
		pthread_mutex_unlock(&compaction_flush_wait);
#endif
		lsm_io_sched_flush();	
		q_dequeue(_this->q);
	}
	
	return NULL;
}

void compaction_check(KEYT key, bool force){
	compR *req;
#ifdef KVSSD
	if(force || unlikely(LSM.memtable->all_length+KEYLEN(key)+sizeof(uint16_t)>PAGESIZE-KEYBITMAP || LSM.memtable->size >= KEYBITMAP/sizeof(uint16_t)))
#else
	if(unlikely(LSM.memtable->size==LSM.FLUSHNUM))
#endif
	{
		req=(compR*)malloc(sizeof(compR));
		req->fromL=-1;
		req->toL=0;
	//	while(LSM.temptable){}

		LSM.temptable=LSM.memtable;
		LSM.memtable=skiplist_init();
		compaction_assign(req);
#ifdef WRITEWAIT
		pthread_mutex_lock(&compaction_flush_wait);
#endif
	}
}

void compaction_htable_read(run_t *ent,PTR* value){
	algo_req *areq=(algo_req*)malloc(sizeof(algo_req));
	lsm_params *params=(lsm_params*)malloc(sizeof(lsm_params));

	params->lsm_type=HEADERR;
	//valueset_assign
	params->value=inf_get_valueset(NULL,FS_MALLOC_R,PAGESIZE);
	params->target=value;
	params->ppa=ent->pbn;
	areq->parents=NULL;
	areq->end_req=lsm_end_req;
	areq->params=(void*)params;
	areq->type_lower=0;
	areq->rapid=false;
	areq->type=HEADERR;

#ifdef NOCPY
	ent->cpt_data->nocpy_table=nocpy_pick(ent->pbn);
#endif
	//printf("R %u\n",ent->pbn);
	LSM.li->read(ent->pbn,PAGESIZE,params->value,ASYNC,areq);
	return;
}

//run_t* compaction_postprocessing(run_t *target){
//	htable *table;
//#ifdef NOCPY
//	table=htable_assign(NULL,true);
///*#if LEVELN==1
//	if(target->cpt_data->nocpy_table)
//		memcpy(table->sets,target->cpt_data->nocpy_table,PAGESIZE);
//	else{
//	#endif
//		memcpy(table->sets,target->cpt_data->sets,PAGESIZE);
//	#if LEVELN==1
//	}
//	#endif*/
//#else
//	table=htable_assign((char*)target->cpt_data->sets,true);
//#endif
//	target->pbn=compaction_htable_write(table,target->key,(char*)target->cpt_data->sets);
//	return target;
//}


void compaction_subprocessing(struct skiplist *top, struct run** src, struct run** org, struct level *des){
	compaction_sub_wait();

	MS(&LSM.timers[3]);
#ifdef STREAMCOMP
	LSM.lop->stream_comp_wait();
#else
	LSM.lop->merger(top,src,org,des);
#endif
	MA(&LSM.timers[3]);

	KEYT key,end;
	run_t* target=NULL;
	MS(&LSM.timers[2]);
	while((target=LSM.lop->cutter(top,des,&key,&end))){
		//target=compaction_postprocessing(target);
		target->pbn=compaction_htable_write(target->cpt_data,target->key,(char*)target->cpt_data->sets);
		if(!LSM.inplace_compaction){
			LSM.lop->insert(des,target);
			LSM.lop->release_run(target);
		}
		//printf("free %p\n",target->cpt_data->sets);
		//htable_free(target->cpt_data);
		target->cpt_data=NULL;
		free(target);
	}
	MA(&LSM.timers[2]);
}

void compaction_lev_seq_processing(level *src, level *des, int headerSize){
#ifdef LEVELCACHING
	if(src->idx<LEVELCACHING){
		run_t **datas;
		int cache_added_size=LSM.lop->cache_get_size(src);
		LSM.lop->cache_comp_formatting(src,&datas);
		cache_size_update(LSM.lsm_cache,LSM.lsm_cache->m_size+cache_added_size);
		for(int i=0;datas[i]!=NULL; i++){
#ifdef BLOOM
			bf_free(datas[i]->filter);
			datas[i]->filter=LSM.lop->making_filter(datas[i],des->fpr);
#endif
			datas[i]->pbn=compaction_htable_write(datas[i]->cpt_data,datas[i]->key,(char*)datas[i]->cpt_data->sets);
			LSM.lop->insert(des,datas[i]);
		}
		free(datas);
		return;
	}
#endif

#ifdef MONKEY
	if(src->m_num!=des->m_num){
		compaction_seq_MONKEY(src,headerSize,des);
		return;
	}
#endif
	run_t *r;
	lev_iter *iter=LSM.lop->get_iter(src,src->start, src->end);
	for_each_lev(r,iter,LSM.lop->iter_nxt){
		LSM.lop->insert(des,r);
	}
}

/*
skiplist *leveling_preprocessing(level * from, level* to){
	skiplist *res=NULL;
	if(from==NULL){
		return LSM.temptable;
	}
	else{
		res=NULL;
	}
	return res;
}*/

extern bool gc_debug_flag;
int level_cnt;
uint32_t leveling(level *from, level* to,leveling_node *lnode, pthread_mutex_t *lock){
#ifdef COMPACTIONLOG
	char log[1024];
#endif
	level *target_origin=to;
	level *target;

	if(to->idx==LEVELN-1 && from->n_num+LSM.lop->cache_get_size(from)+to->n_num > to->m_num){
		target=LSM.lop->init(target_origin->m_num*2, target_origin->idx,target_origin->fpr,false);
	}else{
		target=LSM.lop->init(target_origin->m_num, target_origin->idx,target_origin->fpr,false);
	}

	LSM.c_level=target;
	level *src=NULL;

	level **src_ptr=NULL, **des_ptr=NULL;
	//body=leveling_preprocessing(from,to);
#ifdef LEVELCACHING
	int before,now;
	if(to->idx<LEVELCACHING){
		before=LSM.lop->cache_get_size(to);
		if(from==NULL){
			MS(&LSM.timers[1]);
			LSM.lop->cache_move(to,target);
			LSM.lop->cache_insert(target,lnode->mem);
			MA(&LSM.timers[1]);
		}else{
			src=from;
			LSM.lop->cache_merge(from,to);
			LSM.lop->cache_move(to,target);
			LSM.lop->cache_free(from);
		}

		now=LSM.lop->cache_get_size(target);
#ifdef COMPACTIONLOG
		sprintf(log,"caching noe %d",now);
		DEBUG_LOG(log);
#endif
		cache_size_update(LSM.lsm_cache,LSM.lsm_cache->m_size-(now-before));
		compaction_heap_setting(target,target_origin);
		if(from){
			LSM.lop->move_heap(target,from);	
		}
		goto chg_level;
	}
#endif
	if(from==NULL){
		if(!LSM.lop->chk_overlap(target_origin,lnode->start,lnode->end)){
			compaction_heap_setting(target,target_origin);
#ifdef COMPACTIONLOG
			static int cnt_1=0;
			sprintf(log,"seq - (-1) to %d (%dth)",to->idx,cnt_1++);
			DEBUG_LOG(log);
#endif
			bool target_processed=false;
#ifdef KVSSD
			if(KEYCMP(lnode->start,target_origin->end)>0)
#else
			if(entry->key > target_origin->end)
#endif
			{
				target_processed=true;
				compaction_lev_seq_processing(target_origin,target,target_origin->n_num);
			}

			run_t *entry=LSM.lop->make_run(lnode->start,lnode->end,-1);
			LSM.lop->mem_cvt2table(lnode->mem,entry);
			entry->pbn=compaction_htable_write(entry->cpt_data,entry->key,(char*)entry->cpt_data->sets);//write table
#ifdef NOCPY
			entry->cpt_data=NULL; //the data will be store at nocpy sets, cpt_data will free at lsm_end_req;
#endif
			LSM.lop->insert(target,entry);
			LSM.lop->release_run(entry);
			free(entry);

			if(!target_processed){
				compaction_lev_seq_processing(target_origin,target,target_origin->n_num);
			}
		}
		else{
#ifdef COMPACTIONLOG
			static int cnt_2=0;
			sprintf(log,"rand - (-1) to %d (%dth)",to->idx,cnt_2++);
			DEBUG_LOG(log);
#endif	
			partial_leveling(target,target_origin,lnode,NULL);
			compaction_heap_setting(target,target_origin);
		}
	}else{
		src=from;
		if(!LSM.lop->chk_overlap(target_origin,src->start,src->end)){//if seq
			compaction_heap_setting(target,target_origin);
#ifdef COMPACTIONLOG
			static int cnt_3=0;
			sprintf(log,"seq - %d to %d info:%d,%d max %d,%d (%dth)",from->idx,to->idx,src->n_num,target_origin->n_num,src->m_num,target_origin->m_num,cnt_3++);
			DEBUG_LOG(log);
#endif
			bool target_processed=false;
#ifdef KVSSD
			if(KEYCMP(target_origin->start,src->end)>0)
#else
			if(target_origin->start>src->end)
#endif
			{
				target_processed=true;
				compaction_lev_seq_processing(src,target,src->n_num);
			}
			compaction_lev_seq_processing(target_origin,target,target_origin->n_num);

			if(!target_processed){
				compaction_lev_seq_processing(src,target,src->n_num);
			}
		}
		else{
#ifdef COMPACTIONLOG
			static int cnt_4=0;
			sprintf(log,"rand - %d to %d info:%d,%d max %d,%d (%dth)",from->idx,to->idx,src->n_num,target_origin->n_num,src->m_num,target_origin->m_num,cnt_4++);
			DEBUG_LOG(log);
#endif
			partial_leveling(target,target_origin,NULL,src);
			compaction_heap_setting(target,target_origin);
		}
		LSM.lop->move_heap(target,src);
	}

#if defined(LEVELCACHING) || defined(LEVELEMUL)
chg_level:
#endif
	des_ptr=&LSM.disk[target_origin->idx];

	if(from!=NULL){ 
		int from_idx=from->idx;
		pthread_mutex_lock(&LSM.level_lock[from_idx]);
		src_ptr=&LSM.disk[src->idx];
		*(src_ptr)=LSM.lop->init(src->m_num,src->idx,src->fpr,src->istier);
		LSM.lop->release(src);
		pthread_mutex_unlock(&LSM.level_lock[from_idx]);
	}
	pthread_mutex_lock(lock);
	target->iscompactioning=target_origin->iscompactioning;
	(*des_ptr)=target;
	LSM.lop->release(to);
	pthread_mutex_unlock(lock);
	LSM.c_level=NULL;
	return 1;
}	

#ifdef MONKEY
void compaction_seq_MONKEY(level *t,int num,level *des){
	run_t **target_s;
	LSM.lop->range_find(t,t->start,t->end,&target_s);

	compaction_sub_pre();
	for(int j=0; target_s[j]!=NULL; j++){
		pthread_mutex_lock(&LSM.lsm_cache->cache_lock);
		if(target_s[j]->c_entry){
#ifdef NOCPY
			target_s[j]->cpt_data->nocpy_table=target_s[j]->cache_data->nocpy_table;
#else
			target_s[j]->cpt_data=htable_copy(target_s[j]->cach_data);
#endif
			pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			memcpy_cnt++;
		}
		else{
			pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			target_s[j]->cpt_data=htable_assign(NULL,false);
			compaction_htable_read(target_s[j],(PTR*)&target_s[j]->cpt_data);
		}
		epc_check++;
	}

	compaction_sub_wait();

	for(int k=0; target_s[k]; k++){
		BF *filter=LSM.lop->making_filter(target_s[k],des->fpr);
		run_t *new_ent=LSM.lop->run_cpy(target_s[k]);

		/*filter back*/
		target_s[k]->filter=new_ent->filter;

		new_ent->filter=filter;
		htable_free(target_s[k]->cpt_data);
		LSM.lop->insert(des,new_ent);
		LSM.lop->release_run(new_ent);
	}
	compaction_sub_post();
	free(target_s);
}
#endif
bool flag_value;
uint32_t partial_leveling(level* t,level *origin,leveling_node *lnode, level* upper){
	KEYT start={0,0};
	KEYT end={0,0};
	run_t **target_s=NULL;
	run_t **data=NULL;
	skiplist *skip=lnode?lnode->mem:skiplist_init();
	if(!upper){
#ifndef MONKEY
		start=lnode->start;
		end=lnode->end;
#else
		start=0;
#endif
	}
	else{
		start=upper->start;
		end=upper->end;
	}
	int test_a, test_b;
//#ifdef MONKEY
	test_a=LSM.lop->unmatch_find(origin,start,end,&target_s);
	if(test_a>origin->n_num){
		DEBUG_LOG("fuck!");
	}
	for(int i=0; target_s[i]!=NULL; i++){
		LSM.lop->insert(t,target_s[i]);
		target_s[i]->iscompactioning=4;
	}
	free(target_s);
//#endif

	compaction_sub_pre();

	if(!upper){
		test_b=LSM.lop->range_find_compaction(origin,start,end,&target_s);
		if(!(test_a | test_b)){
			DEBUG_LOG("can't be");
		}
		upper_table=0;
#ifdef STREAMCOMP
		LSM.lop->stream_merger(skip,NULL,target_s,t);
#endif
		for(int j=0; target_s[j]!=NULL; j++){
			pthread_mutex_lock(&LSM.lsm_cache->cache_lock);

			if(target_s[j]->c_entry){
				memcpy_cnt++;
				cache_entry_lock(LSM.lsm_cache,target_s[j]->c_entry);
#ifndef NOCPY
				target_s[j]->cpt_data=htable_copy(target_s[j]->cache_data);
				target_s[j]->cpt_data->done=true;
#endif//when the data is cached, the data will be loaded from memory in merger logic
				pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			}
			else{
				pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
				target_s[j]->cpt_data=htable_assign(NULL,false);
				target_s[j]->cpt_data->iscached=0;
				compaction_htable_read(target_s[j],(PTR*)&target_s[j]->cpt_data);
			}
			if(!target_s[j]->iscompactioning){
				target_s[j]->iscompactioning=true;
			}
			epc_check++;
		}

		compaction_subprocessing(skip,NULL,target_s,t);

		for(int j=0; target_s[j]!=NULL; j++){
			if(target_s[j]->iscompactioning!=3){
				invalidate_PPA(target_s[j]->pbn);//invalidate_PPA
			}
			if(target_s[j]->c_entry){
				cache_entry_unlock(LSM.lsm_cache,target_s[j]->c_entry);
/*#ifdef NOCPY
				if(target_s[j]->cpt_data->iscached==1 && !target_s[j]->cache_data){
					free(target_s[j]->cpt_data->nocpy_table);
				}
#endif*/
			}
			else{	
				htable_free(target_s[j]->cpt_data);
				target_s[j]->cpt_data=NULL;
			}
		}
		free(target_s);
	}
	else{
		int src_num, des_num; //for stream compaction
		des_num=LSM.lop->range_find_compaction(origin,start,end,&target_s);//for stream compaction
#ifdef LEVELCACHING
		if(upper->idx<LEVELCACHING){
			//for caching more data
			int cache_added_size=LSM.lop->cache_get_size(upper);
			cache_size_update(LSM.lsm_cache,LSM.lsm_cache->m_size+cache_added_size);
			src_num=LSM.lop->cache_comp_formatting(upper,&data);
		}
		else{
#endif
			src_num=LSM.lop->range_find_compaction(upper,start,end,&data);	
#ifdef LEVELCACHING
		}
#endif
		if(src_num && des_num == 0 ){
			printf("can't be\n");
			abort();
		}
#ifdef STREAMCOMP
		LSM.lop->stream_merger(NULL,data,target_s,t);
#endif
		for(int i=0; target_s[i]!=NULL; i++){
			run_t *temp=target_s[i];
			if(temp->iscompactioning==4){
				continue;
			}
			if(!temp->iscompactioning) temp->iscompactioning=true;
			pthread_mutex_lock(&LSM.lsm_cache->cache_lock);		
			if(temp->c_entry){
				cache_entry_lock(LSM.lsm_cache,temp->c_entry);
#ifndef NOCPY
				temp->cpt_data=htable_copy(temp->cache_data);
				temp->cpt_data->done=true;
#endif
				memcpy_cnt++;
				pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			}
			else{
				pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
				temp->cpt_data=htable_assign(NULL,false);
				temp->cpt_data->iscached=0;
				compaction_htable_read(temp,(PTR*)&temp->cpt_data);
			}
			//htable_check(temp->cpt_data,8499,81665);
			epc_check++;
		}
#ifdef LEVELCACHING
		if(upper->idx<LEVELCACHING){
			goto skip;
		}
#endif
		for(int i=0; data[i]!=NULL; i++){
			run_t *temp=data[i];
			temp->iscompactioning=true;
			pthread_mutex_lock(&LSM.lsm_cache->cache_lock);
			
			if(temp->c_entry){
				cache_entry_lock(LSM.lsm_cache,temp->c_entry);
#ifndef NOCPY
				temp->cpt_data=htable_copy(temp->cache_data);
				temp->cpt_data->done=true;
#endif
				memcpy_cnt++;
				pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			}
			else{
				pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
				temp->cpt_data=htable_assign(NULL,false);
				temp->cpt_data->iscached=0;
				compaction_htable_read(temp,(PTR*)&temp->cpt_data);
			}
			epc_check++;
		}
skip:
		compaction_subprocessing(NULL,data,target_s,t);

		for(int i=0; data[i]!=NULL; i++){
			run_t *temp=data[i];

			if(temp->iscompactioning!=3 && temp->pbn!=UINT_MAX)
				invalidate_PPA(temp->pbn);
			
			if(temp->c_entry){
				cache_entry_unlock(LSM.lsm_cache,temp->c_entry);
			}else{
				htable_free(temp->cpt_data);
				temp->cpt_data=NULL;
			}
		}

		for(int i=0; target_s[i]!=NULL; i++){	
			run_t *temp=target_s[i];
			if(temp->iscompactioning==4) continue;

			if(temp->iscompactioning!=3)
				invalidate_PPA(temp->pbn);

			if(temp->c_entry){
				cache_entry_unlock(LSM.lsm_cache,temp->c_entry);
			}else{
				htable_free(temp->cpt_data);
				temp->cpt_data=NULL;
			}
		}
		free(data);
		free(target_s);
	}
	compaction_sub_post();
	if(!lnode) skiplist_free(skip);
	return 1;
}

#if (LEVELN==1)
#define MAPNUM(a) (a/FULLMAPNUM)
#define MAPOFFSET(a) (a%FULLMAPNUM)
void level_one_header_update(run_t *pre_run){
	uint32_t old_header_pbn=pre_run->pbn;
	if(!pre_run->c_entry && pre_run->cpt_data && cache_insertable(LSM.lsm_cache)){

		char *cache_temp;
		if(pre_run->cpt_data->nocpy_table){
			cache_temp=(char*)pre_run->cpt_data->nocpy_table;
		}else{
			cache_temp=(char*)pre_run->cpt_data->sets;
		}

#ifdef NOCPY
		pre_run->cache_data=htable_dummy_assign();
		pre_run->cache_data->nocpy_table=cache_temp;
#else
		pre_run->cache_data=htable_copy(pre_run->cpt_data);
#endif
		pre_run->c_entry=cache_insert(LSM.lsm_cache,pre_run,0);
	}
	compaction_postprocessing(pre_run);
	if(old_header_pbn!=UINT_MAX && pre_run->iscompactioning!=3)
		invalidate_PPA(old_header_pbn);
	if(pre_run->iscompactioning==5){
		free(pre_run->cpt_data->sets);
		pre_run->cpt_data->sets=NULL;
	}
	pre_run->iscompactioning=0;
	htable_free(pre_run->cpt_data);
	pre_run->cpt_data=NULL;

}

uint32_t level_one_processing(level *from, level *to, run_t *entry, pthread_mutex_t *lock){
	skiplist *body;
	level *new_level=LSM.lop->init(to->m_num,to->idx,to->fpr,false);
	LSM.c_level=new_level;
	body=leveling_preprocessing(from,to);

	snode *node;
	run_t *table;
	uint32_t pre_map_num=UINT_MAX;
	//printf("compaction_called\n");
	compaction_sub_pre();
	for_each_sk(node,body){ //read data
		uint32_t mapnum=MAPNUM(node->key);
		if(pre_map_num==mapnum) continue;
		table=&to->mappings[mapnum];
		if(table->pbn==UINT_MAX) {
			table->cpt_data=htable_assign(NULL,false);
			table->cpt_data->sets=(keyset*)malloc(PAGESIZE);
			memset(table->cpt_data->sets,-1,PAGESIZE);
			table->iscompactioning=5;//new runt
			pre_map_num=mapnum;
			continue;
		}
		pthread_mutex_lock(&LSM.lsm_cache->cache_lock);
		if(table->c_entry){
			memcpy_cnt++;
#ifdef NOCPY
			table->cpt_data=htable_dummy_assign();
			table->cpt_data->nocpy_table=table->cache_data->nocpy_table;
#else
			table->cpt_data=htable_copy(table->cache_data);
#endif
			pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			table->cpt_data->done=true;
		}
		else{
			pthread_mutex_unlock(&LSM.lsm_cache->cache_lock);
			table->cpt_data=htable_assign(NULL,false);
			compaction_htable_read(table,(PTR*)&table->cpt_data);	
		}

		if(!table->iscompactioning){
			table->iscompactioning=1;
		}
		epc_check++;
		pre_map_num=mapnum;
	}
	compaction_sub_wait(); //read req done
	
	pre_map_num=UINT_MAX;
	uint32_t old_header_pbn;
	run_t * pre_run=NULL;

	for_each_sk(node,body){
		uint32_t mapnum=MAPNUM(node->key);
		uint32_t offset=MAPOFFSET(node->key);
		table=&to->mappings[mapnum];

		keyset *map;
#ifdef NOCPY
		if(table->iscompactioning==5){
			map=(keyset*)table->cpt_data->sets;
		}else{
			map=(keyset*)table->cpt_data->nocpy_table;
		}
#else
		map=table->cpt_data->sets;
#endif

		if(map[offset].ppa!=UINT_MAX){
			invalidate_PPA(map[offset].ppa);
		}
		map[offset].lpa=node->key;
		map[offset].ppa=node->ppa;

		LSM.lop->range_update(new_level,NULL,node->key);

		if(pre_map_num!=UINT_MAX &&pre_map_num!=mapnum){
			level_one_header_update(pre_run);
		}
		pre_run=table;
		pre_map_num=mapnum;
	}
	
	level_one_header_update(pre_run);

	pthread_mutex_lock(&LSM.entrylock);
	LSM.tempent=NULL;
	LSM.lop->release_run(entry);
	pthread_mutex_unlock(&LSM.entrylock);

	compaction_sub_post();

	run_t *fr,*tr;
	for(int i=0; i<to->n_num; i++){
		tr=&new_level->mappings[i];
		fr=&to->mappings[i];
	
		tr->key=fr->key;
		tr->end=fr->end;
		tr->pbn=fr->pbn;

		tr->c_entry=NULL;
		tr->cache_data=NULL;

		tr->isflying=0;
		tr->req=NULL;
		tr->cpt_data=NULL;
		tr->iscompactioning=false;
	}
	compaction_heap_setting(new_level,to);

	level **des_ptr=&LSM.disk[to->idx];
	pthread_mutex_lock(lock);
	new_level->iscompactioning=to->iscompactioning;
	(*des_ptr)=new_level;
	LSM.lop->release(to);
	pthread_mutex_unlock(lock);

	return 1;
}
#endif

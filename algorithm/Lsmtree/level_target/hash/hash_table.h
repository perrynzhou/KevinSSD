#ifndef __H_HASH_H__
#define __H_HASH_H__
#define LOADF 0.9f
#define HENTRY (FULLMAPNUM-1)
#define CUC_ENT_NUM ((int)(HENTRY*LOADF))

#include <stdint.h>
#include <sys/types.h>
#include "../../skiplist.h"
#include "../../level.h"

inline KEYT f_h(KEYT a){ return (a*2654435761);}
typedef struct hash{
	uint32_t n_num;
	uint32_t t_num;
	keyset b[HENTRY];
}hash;

typedef struct hash_body{
	level *lev;
	run_t *temp;
	run_t *late_use_node;
	run_t *late_use_nxt;
	skiplist *body;
}hash_body;

typedef struct hash_iter_data{
	snode *now_node;
	snode *end_node;
	int idx;
}hash_iter_data;

level* hash_init(int size, int idx, float fpr, bool istier);
void hash_free(level*);
void hash_insert(level *, run_t*);
keyset* hash_find_keyset(char *data,KEYT lpa);
run_t *hash_make_run(KEYT start, KEYT end, KEYT pbn);
run_t **hash_find_run( level*,KEYT);
uint32_t hash_range_find( level *,KEYT, KEYT,  run_t ***);
uint32_t hash_unmatch_find( level *,KEYT, KEYT,  run_t ***);
lev_iter* hash_get_iter( level *,KEYT start, KEYT end);
run_t * hash_iter_nxt( lev_iter*);
KEYT h_max_table_entry();


void hash_free_run( run_t*);
run_t* hash_run_cpy( run_t *);

htable *hash_mem_cvt2table(skiplist*);
void hash_merger( skiplist*,  run_t**,  run_t**,  level*);
htable *hash_cutter( skiplist*,  level*, int* end_idx);

bool hash_chk_overlap( level *, KEYT, KEYT);
void hash_overlap(void *);
void hash_tier_align( level *);
void hash_print(level *);
void hash_all_print();
#endif

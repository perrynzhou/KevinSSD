#ifndef __RUN_A_H__
#define __RUN_A_H__
#include "../../include/container.h"
#include "../../include/settings.h"
#include "../../include/lsm_settings.h"
#include "lsmtree.h"

struct htable;
struct skiplis;
typedef struct Entry{
	KEYT key;
	KEYT end;
	KEYT pbn;
	uint8_t *bitset;
	uint64_t version;
#ifdef BLOOM
	BF *filter
#endif
	struct htable *t_table;
	struct skiplist *t_skip;
}Entry;

typedef struct Node{
	int n_num;
	int m_num;
	int e_size;
	int start;
	int end;
	char **body_addr;
	char *body;
}Node;

typedef struct level{
	int size;
	int r_num;
	int r_n_num;
	int m_num;//number of entries
	int n_num;
	int entry_p_run;
	int r_size;//size of run
	float fpr;
	bool isTiering;
	KEYT start;
	KEYT end;
	pthread_mutex_t level_lock;
	char *body;
}level;

typedef struct iterator{
	level *lev;
	Node *now;
	Entry *v_entry;
	int r_idx;
	int idx;
	bool flag;
}Iter;
Entry *level_make_entry(KEYT,KEYT,KEYT);
Entry* level_entcpy(Entry *src,char *des);
Entry *level_entry_copy(Entry *src);
level *level_init(level *,int size,bool);
level *level_clear(level *);
level *level_copy(level *);//do coding
Entry **level_find(level *,KEYT key);
Entry *level_find_fromR(Node *, KEYT key);
int level_range_find(level *,KEYT start, KEYT end, Entry ***target);
bool level_check_overlap(level*,KEYT start, KEYT end);
bool level_full_check(level *);
Node *level_insert(level *,Entry*);
Entry *level_get_next(Iter *);
Iter *level_get_Iter(level *);
void level_print(level *);
void level_free(level *);

Node *ns_run(level*, int );
Entry *ns_entry(Node *,int);
#endif

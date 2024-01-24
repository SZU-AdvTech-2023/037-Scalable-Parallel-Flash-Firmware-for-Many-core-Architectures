#ifndef _LIST_H_
#define _LIST_H_

struct list_head {
    struct list_head *prev, *next;
};

#define LIST_INIT(name)  \
    {                    \
        &(name), &(name) \
    }
#define DEF_LIST(name) struct list_head name = LIST_INIT(name)
#define INIT_LIST_HEAD(ptr)  \
    do {                     \
        (ptr)->next = (ptr); \
        (ptr)->prev = (ptr); \
    } while (0)

/*
 * Dzl：
 * Get the entry that is associated with the list pointer.
 */
#define list_entry(ptr, type, member)                     \
    ({                                                    \
        const typeof(((type*)0)->member)* __mptr = (ptr); \
        (type*)((char*)__mptr - offsetof(type, member));  \
    })
#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)
#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, typeof(*(pos)), member)

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE*)0)->MEMBER)
#endif

static inline int list_empty(const struct list_head* list);
static inline void list_add(struct list_head* new, struct list_head* head);
static inline void list_del(struct list_head* node);

#define prefetch(x) __builtin_prefetch(&x)

//Can not delete pos
#define list_for_each_entry(pos, head, member)                 \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
         prefetch(pos->member.next), &(pos->member) != (head); \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)       \
    for (pos = list_first_entry(head, typeof(*pos), member), \
        n = list_next_entry(pos, member);                    \
         &pos->member != (head); pos = n, n = list_next_entry(n, member))

static inline int list_empty(const struct list_head* list)
{
    return (list->next == list);
}

static inline void __list_add(struct list_head* new, struct list_head* pre,
                              struct list_head* next)
{
    new->prev = pre;
    new->next = next;
    pre->next = new;
    next->prev = new;
}

static inline void list_add(struct list_head* new, struct list_head* head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head* new, struct list_head* head)
{
    __list_add(new, head->prev, head);
}

static inline void list_del(struct list_head* node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;

    node->prev = node;
    node->next = node;
}

static inline void __list_splice(const struct list_head* list,
                                 struct list_head* prev, struct list_head* next)
{
    struct list_head* first = list->next;
    struct list_head* last = list->prev;

    first->prev = prev;
    prev->next = first;

    last->next = next;
    next->prev = last;
}

static inline void list_splice(const struct list_head* list,
                               struct list_head* head)
{
    if (!list_empty(list)) __list_splice(list, head, head->next);
}

static inline void list_splice_init(struct list_head* list,
                                    struct list_head* head)
{
    if (!list_empty(list)) {
        __list_splice(list, head, head->next);
        INIT_LIST_HEAD(list);
    }
}

#endif
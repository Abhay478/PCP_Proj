// #include <iostream>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include <string>
#include <atomic>

using namespace std;
#define BUF_SIZE 32
#define STUB_GEN 0xffffffe0

#define MARKLEAF(node) (Node *)(((long long)(node)) | 1)
#define ADDRESS(node) (Node *)(((long long)(node)) & ~3) // Last two bits.
#define LEAF(node) ((long long)(node) & 1)
#define IS_EMPTY(buf) buf & ((1 << BUF_SIZE) - 1) == 0
#define GOLEFT(cnode, pnode, ptrp) cNode = pNode->left; ptrp = ChildType::LEFT;
#define GORIGHT(cnode, pnode, ptrp) cNode = pNode->right; ptrp = ChildType::RIGHT;
#define \
TRAVERSE(key, cNode, pNode, ptrp) \
    if (key <= pNode->base && !IS_EMPTY(pNode->buf)) \
        {GOLEFT(cNode, pNode, ptrp)} \
    else \
        {GORIGHT(cNode, pNode, ptrp)} \


#define IN_RANGE(node, key) (node->base <= key && key < node->base + BUF_SIZE)

enum class InsertStatus {
    Success,
    Duplicate,
    Deleted
};

struct Node {
    int base;
    atomic_int buf; // bitvector
    atomic<Node *> next;
    atomic<Node *> left;
    atomic<Node *> right;
    Node * parent;
    ChildType ptrp;
    bool inserting;

    bool del; // If no one is deleting from this node, batch inserts? Fetch-Or maybe?

    Node(
        int base, 
        int buf, 
        Node * next = NULL, 
        Node * left = NULL, 
        Node * right = NULL, 
        Node * parent = NULL, 
        ChildType ptrp, 
        bool inserting = true
    ) : base(base), 
        buf(buf), 
        next(next), 
        left(left), 
        right(right), 
        parent(parent), 
        ptrp(ptrp) {}

    void dbg() {
        int u = 1 << 31;

        for(int c = 0; u; c++) {
            if (buf & u) {
                printf("%d ", base + c);
            } 
            u >>= 1;
        }
    }

    InsertStatus insert(int k);
    // int deleteMin();
} * leaf = (Node *)1;

thread_local Node * prev_head = NULL, * dummy = NULL;

enum class ChildType {
    LEFT,
    RIGHT
};


struct Seek {
    Node * exists;
    atomic<Node *> * pred; // Coz we want to access the actual field of the node, which is a Node *.
    Node * succ;
    Node * par;
    ChildType ptrp;
};

struct Sentinel {
    Node * root;
    Node * head;

    Sentinel() : root(NULL), head(NULL) {}
};

struct RangeQueue {
    Sentinel * sentinel;

    RangeQueue() {sentinel = new Sentinel();}

    InsertStatus insert(int k);
    int delete_min();

    void dbg();

    private:
    Seek insert_search(int k);
    int clean_tree(Node * dummy);
};

InsertStatus RangeQueue::insert(int k) {
    while(1) {
        Seek s = insert_search(k); 
        if (s.exists) {
            auto q = s.exists->insert(k); // Put all the cas-ing in this.
            if (q == InsertStatus::Deleted) {
                continue;
            } 
            return q;
        }

        Node * newNode = new Node(
            k & STUB_GEN, 
            1 << (BUF_SIZE - 1 - k%BUF_SIZE), 
            s.succ, 
            MARKLEAF(s.pred->load()), 
            NULL, 
            s.par, 
            s.ptrp
        );
        newNode->right = MARKLEAF(newNode);

        if (!s.pred->compare_exchange_strong(s.succ, newNode)) {
            continue;
        }

        if (s.ptrp == ChildType::LEFT) {
            s.par->left.compare_exchange_strong(leaf, newNode);
        } else {
            s.par->right.compare_exchange_strong(leaf, newNode);
        }

        return InsertStatus::Success;
    }
}

Seek RangeQueue::insert_search(int k) {
    Seek s;
    Node * cNode = sentinel->root;
    Node * pNode = NULL;
    ChildType ptrp;

    while(1) {
        if(pNode && IS_EMPTY(pNode->buf)) {
            GORIGHT(cNode, pNode, ptrp);
            Node * mnode = pNode;
            while(1) {
                if(IS_EMPTY(pNode->buf)) {
                    if(!LEAF(cNode)) {
                        pNode = cNode;
                    }
                    else {
                        pNode = cNode->next;
                        GORIGHT(cNode, pNode, ptrp);
                        break;
                    }
                }
                else {
                    // TODO: Probabilistic stuff
                    TRAVERSE(k, cNode, pNode, ptrp)
                    
                    break;
                }
            }

            continue;
        }
        if (cNode == NULL) {
            s.exists = NULL;
            s.pred = NULL;
            s.succ = NULL;
            s.par = pNode;
            s.ptrp = ptrp;
            return s;
        }

        if (cNode == leaf) {
            s.exists = NULL;
            s.pred = NULL;
            s.succ = cNode;
            s.par = pNode;
            s.ptrp = ptrp;
            return s;
        }

        if (IN_RANGE(cNode, k)) {
            s.exists = cNode;
            s.pred = &cNode->next;
            s.succ = cNode->next;
            s.par = pNode;
            s.ptrp = ptrp;
            return s;
        }

        if (k < cNode->base) {
            GOLEFT(cNode, pNode, ptrp);
        } else {
            GORIGHT(cNode, pNode, ptrp);
        }
    }
}

// Barebones, copied from pseudocode without much brain usage. 
// What even is prev_dummy?
int RangeQueue::delete_min() {
    auto hnode = sentinel->head;
    if(prev_head == hnode) {
        dummy = prev_dummy;
    } else {
        prev_head = hnode;
        dummy = hnode;
    }

    while(1) {
        auto next_leaf = dummy->next.load();
        if (!next_leaf) {
            return -1;
        } 
        if(IS_EMPTY(next_leaf->buf)) {
            dummy = next_leaf;
            continue;
        }

        auto buf = next_leaf->buf.load();
        auto lbm = buf & -buf; // What?
        auto prev = next_leaf->buf.fetch_and(~lbm);
        if(!prev) continue;

        auto min = __builtin_ctz(lbm) + next_leaf->base; // Count trailing zeros. IBM builtin. Copilot thing.

        if(prev != lbm /*or probability stuff*/) {
            return min;
        }

        if(sentinel->head->next.compare_exchange_strong(hnode, ADDRESS(next_leaf))) {
            clean_tree(next_leaf);
        }

        return min;


    }
}
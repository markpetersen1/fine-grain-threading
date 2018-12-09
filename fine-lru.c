/* -*- mode:c; c-file-style:"k&r"; c-basic-offset: 4; tab-width:4; indent-tabs-mode:nil; mode:auto-fill; fill-column:78; -*- */
/* vim: set ts=4 sw=4 et tw=78 fo=cqt wm=0: */

/* @* Place your name here, and any other comments *@
        Mark Petersen
 * @* that deanonymize your work inside this syntax *@
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "lru.h"

#include <errno.h>

/* Define the simple, singly-linked list we are going to use for tracking lru */
struct list_node {
    struct list_node* next;
    int key;
    int refcount;
    // Protects this node's contents
    pthread_mutex_t mutex;
};

static struct list_node* list_head = NULL;

/* A static mutex; protects the count and head.
 * XXX: We will have to tolerate some lag in updating the count to avoid
 * deadlock. */
static pthread_mutex_t mutex;
static int count = 0;
static pthread_cond_t cv_low, cv_high;

static volatile int done = 0;

/* Initializes the global mutex and mutexes for each condition variable. */
int init (int numthreads) { 
    if ((errno = pthread_mutex_init(&mutex, NULL)) != 0) {
        return -errno;
    }
    if ((errno = pthread_cond_init(&cv_low, NULL)) != 0) {
        return -errno;
    }
    if ((errno = pthread_cond_init(&cv_high, NULL)) != 0) {
        return -errno;
    }
    return 0;
}

/* Return 1 on success, 0 on failure.
 * Should set the reference count up by one if found; add if not.
 * 
 * Reference described in more detail in README */
int reference (int key) {
    pthread_mutex_lock(&mutex); //lock the entire mutex so that we can safely wait and also for initial initializations
    
    while (count >= HIGH_WATER_MARK) { //continue running while we are not in the "ideal" upper watermark range
        pthread_cond_wait(&cv_low, &mutex); //wait here until we get the signal from clean to check if the count dropped to below this upper threshold
    }

    int found = 0;
    struct list_node* cursor = list_head;
    struct list_node* last = NULL;

    if (list_head != NULL) { //only for the very time reference is called and we haven't initialized any nodes yet
        pthread_mutex_lock(&(list_head->mutex)); //lock head so we can start our "hand-over-hand" locking paradigm
    }

    pthread_mutex_unlock(&mutex); //unlock on mutex so that other threads can now acquire it

    while(cursor) {

        if (cursor->key < key) {
            //Invariant here: we have two consecutive nodes that are both locked aside from first iteration of list
            if (last != NULL) { //accounts for first iteration where last is null - this will be true in all other cases
                pthread_mutex_unlock(&(last->mutex)); //unlock the first of the two consecutive locks
            }
            last = cursor;
            cursor = cursor->next;
            if (cursor != NULL) { //accounts for potential last iteration where cursor makes it to end of list and is null - true in all other cases
                pthread_mutex_lock(&(cursor->mutex)); //bring back second lock one node forward so we still have two consecutive lock and maintain the paradigm
            }
        } else {
            if (cursor->key == key) {
                pthread_mutex_lock(&mutex); //non-atomical instructions so lock it
                cursor->refcount++;
                found++;
                pthread_mutex_unlock(&mutex);
                if (last != NULL) { //edge case for when input key is the first one in list
                    pthread_mutex_unlock(&(last->mutex)); //we are done with last at this point, so we go ahead and unlock it
                }
            }
            pthread_mutex_unlock(&(cursor->mutex)); //done with cursor - go ahead and unlock it so other threads can access this node
                                                    //we want to unlock cursor regardless of whether or not we found the key
            break;
        }
    }

    if (!found) {
        // Handle 2 cases: the list is empty/we are trying to put this at the
        // front, and we want to insert somewhere in the middle or end of the
        // list.
        // Invariant: We have at most one lock here. Either previous, current, or both of these will equal null
        pthread_mutex_lock(&mutex); //non-atomical instruction of counting and mallocing so lock it
        struct list_node* new_node = malloc(sizeof(struct list_node));
        pthread_mutex_init(&(new_node->mutex), NULL); //initialize the new node's mutex
        if (!new_node) return 0;
        count++;
        new_node->key = key;
        new_node->refcount = 1;
        new_node->next = cursor;
        pthread_mutex_unlock(&mutex);
        pthread_mutex_lock(&(new_node->mutex)); //lock up the new node's mutex for the upcoming assignments

        if (last == NULL) {
            list_head = new_node; //key value is less than first node in list - beginning of list
                                  //don't need to unlock last's mutex because last is null here
        }
        else {
            last->next = new_node; //we traversed list up till (cursor->key < key) returned false - insert here
            pthread_mutex_unlock(&(last->mutex)); //in the most general case, this will be locked from the earlier part of reference
                                                  //we want to be sure to unlock last so we don't have dangling locks 
        }
        pthread_mutex_unlock(&(new_node->mutex)); //unlock the new node's mutex, thereby guarenteeing that everything is unlocked here
    }

    pthread_mutex_lock(&mutex); //as per rule #3 of basic thread programming rules (holding a lock when utilizing condition variables) 
    if (count > LOW_WATER_MARK) { //we are above the lower threshold watermark, so signal to the lock in clean to continue on
        pthread_cond_signal(&cv_high);
    }
    pthread_mutex_unlock(&mutex);

    return 1;
}

/* Do a pass through all elements, either clear the reference bit,
 * or remove if it hasn't been referenced since last cleaning pass.
 *
 * check_water_mark: If 1, block until there are more elements in the cache
 * than the LOW_WATER_MARK.  This should only be 0 during self-testing or in
 * single-threaded mode.
 * 
 * Clean described in more detail in README
 */
void clean(int check_water_mark) {
    pthread_mutex_lock(&mutex); //locking entire mutex so that our initalizations stay deterministic, as well as for the condition variables

    if (check_water_mark == 1) { //flag for whether we care about watermarks
        while (count <= LOW_WATER_MARK) {
            pthread_cond_wait(&cv_high, &mutex);
        }
    }

    if (list_head != NULL) {
        pthread_mutex_lock(&(list_head->mutex)); //lock the head so that we can initialize cursor and begin our "hand-over-hand" locking paradigm
    }
    
    struct list_node* cursor = list_head;
    struct list_node* last = NULL;
    pthread_mutex_unlock(&mutex); //give up mutex for other threads

    while(cursor) {
        cursor->refcount--; //cursor is already locked so we are safe to decrement
        if (cursor->refcount == 0) {
            struct list_node* tmp = cursor;
            if (last) {
                last->next = cursor->next;
                pthread_mutex_unlock(&(last->mutex)); //unlock last for a bit for other threads to use 
            } else {
                list_head = cursor->next;
            }
            tmp = cursor->next;
            if (tmp != NULL) { //accounts for edge case of being end of list
                pthread_mutex_lock(&(tmp->mutex)); //want to keep hand-in-hand locking paradigm, so lock next node to get it set up
            }
            free(cursor); //this also "removes" the lock associated with this node - so we are getting rid of one of our two locks here
            cursor = tmp;
            pthread_mutex_lock(&mutex); //lock global thread for non-atomical operation
            count--;
            pthread_mutex_unlock(&mutex);
            if (last != NULL) { //if this is end of list (cursor == null and last == null) then we have effectively removed all locks by this point 
                pthread_mutex_lock(&(last->mutex)); //adding our second lock as long as we are not at beginning of list
            }
        } else {
            //Invariant at this point: two consecutive nodes should be locked unless cursor is the list_head (this is same as in reference)
            if (last != NULL) { //again for the first iteration of this while loop
                pthread_mutex_unlock(&(last->mutex)); //unlock one lock and move last and cursor one node over
            }
            last = cursor;
            cursor = cursor->next;
            if (cursor != NULL) { //again accounting for case where cursor is at end of list
                pthread_mutex_lock(&(cursor->mutex)); //bring back second lock one node forward so we still have two consecutive locks
            }
        }
    }

    if (last != NULL) {
        pthread_mutex_unlock(&(last->mutex)); //in case the while loop never runs
    }

    pthread_mutex_lock(&mutex);
    if (count < HIGH_WATER_MARK) { //as per rule #3 of basic thread programming rules (holding a lock when utilizing condition variables)
        pthread_cond_signal(&cv_low);
    }
    pthread_mutex_unlock(&mutex);
}


/* Optional shut-down routine to wake up blocked threads.
   May not be required.

   Described in README */
void shutdown_threads () { //block any threads that are blocked on this condition
    pthread_mutex_lock(&mutex);
    pthread_cond_broadcast(&cv_low);
    pthread_cond_broadcast(&cv_high);
    pthread_mutex_unlock(&mutex);
}

/* Print the contents of the list.  Mostly useful for debugging. */
void print () {
    pthread_mutex_lock(&mutex);
    printf("=== Starting list print ===\n");
    printf("=== Total count is %d ===\n", count);
    struct list_node* cursor = list_head;
    while(cursor) {
        printf ("Key %d, Ref Count %d\n", cursor->key, cursor->refcount);
        cursor = cursor->next;
    }
    printf("=== Ending list print ===\n");
    pthread_mutex_unlock(&mutex);
}

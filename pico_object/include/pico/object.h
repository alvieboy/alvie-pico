/*
 * Copyright (c) 2022 Alvaro Lopes
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_OBJECT_H
#define _PICO_OBJECT_H

#include "pico.h"
#include "pico/sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TO_PICO_OBJECT(x) ((pico_object_t*)(x))

#undef PICO_DEBUG_OBJECTS

// Object debugging.
#ifdef PICO_DEBUG_OBJECTS
#define DBG(x...) printf(x)
#else
#define DBG(x...)
#endif

struct pico_object__;

typedef void (*pico_object_dealloc_func_t)(struct pico_object__ *object);

typedef struct pico_object__
{
    critical_section_t critical_section;
    pico_object_dealloc_func_t dealloc;
    uint8_t refcnt;
} pico_object_t;


static inline void pico_object_init(pico_object_t *object, pico_object_dealloc_func_t dealloc_func);
static inline void pico_object_init_noref(pico_object_t *object, pico_object_dealloc_func_t dealloc_func);
static inline pico_object_t *pico_object_ref(pico_object_t *object);
/*
 Return object if the object is still alive, NULL otherwise
 */
static inline pico_object_t *pico_object_unref(pico_object_t *object);


static inline void pico_object_init_noref(pico_object_t *object, pico_object_dealloc_func_t dealloc_func)
{
    critical_section_init(&object->critical_section);
    object->refcnt = 0;
    object->dealloc = dealloc_func;
}

static inline void pico_object_init(pico_object_t *object, pico_object_dealloc_func_t dealloc_func)
{
    pico_object_init_noref(object, dealloc_func);
    object->refcnt = 1;
}

static inline pico_object_t *pico_object_ref(pico_object_t *object)
{
    if (object!=NULL)
    {
        critical_section_enter_blocking(&object->critical_section);
        assert(object->refcnt != 255);
        object->refcnt++;
        DBG("OBJECT: ref %p -> %d\n", object, object->refcnt);
        critical_section_exit(&object->critical_section);
    }
    return object;
}

static inline pico_object_t *pico_object_ref_nolock(pico_object_t *object)
{
    if (object!=NULL)
    {
        assert(object->refcnt != 255);
        object->refcnt++;
        DBG("OBJECT: ref %p -> %d\n", object, object->refcnt);
    }
    return object;
}

static inline pico_object_t *pico_object_unref(pico_object_t *object)
{
    if (object) {
        critical_section_enter_blocking(&object->critical_section);
        assert(object->refcnt != 0);
        uint8_t newref = --object->refcnt;
        DBG("OBJECT: unref %p -> %d\n", object, object->refcnt);
        critical_section_exit(&object->critical_section);

        // This is racy. Fix

        //printf("%s: Unref %p\n", __func__, object);
        if (newref==0) {
            //  printf("%s: Object %p bye bye\n", __func__, object);
            object->dealloc(object);
            object = NULL;
        }
    }
    return object;
}

static inline pico_object_t *pico_object_unref_nolock(pico_object_t *object)
{
    if (object) {
        assert(object->refcnt != 0);
        uint8_t newref = --object->refcnt;
        DBG("OBJECT: ref %p -> %d\n", object, object->refcnt);
        //printf("%s: Unref %p\n", __func__, object);
        if (newref==0) {
            //  printf("%s: Object %p bye bye\n", __func__, object);
            // This is racy. Fix

            object->dealloc(object);
            object = NULL;
        }
    }
    return object;
}

static inline void pico_object_lock(pico_object_t *object)
{
    critical_section_enter_blocking(&object->critical_section);
}

static inline void pico_object_unlock(pico_object_t *object)
{
    critical_section_exit(&object->critical_section);
}

#ifdef __cplusplus
}
#endif

#endif

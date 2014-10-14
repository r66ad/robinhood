/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2009, 2010 CEA/DAM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "status_manager.h"
#include "rbh_misc.h"
#include "rbh_logs.h"
#include "Memory.h"

#include "../modules/lhsm.h" /** FIXME drop this when dynamic module management is implemented */
#include "../modules/backup.h" /** FIXME drop this when dynamic module management is implemented */

/** list of loaded status managers */
static status_manager_t **sm_cache = NULL;
static unsigned int sm_count = 0;

/** list of status manager instances */
static sm_instance_t **sm_inst = NULL;
unsigned int sm_inst_count = 0; /* must be available from other modules to handle attribute masks */

void sm_status_ensure_alloc(char const ***p_tab)
{
    if (*p_tab == NULL)
        *p_tab = MemCalloc(sm_inst_count, sizeof(char *));
}

void sm_status_free(char const ***p_tab)
{
    if (*p_tab != NULL)
    {
        MemFree(*p_tab);
        *p_tab = NULL;
    }
}

/* contents of status_manager:
name, flags, status_enum, status_count, status_needs_attrs_cached,
status_needs_attrs_fresh, get_status_func, changelog_cb.

For status manager that handle removed entries the 2 masks are:
    - to determine if the entry is to be saved in softrm table
    - fields to save in softrm table (needed for undelete or recovery)
*/

/* -------------- Basic status manager implementation ------------------- */

#define BASIC_ST_COUNT 2
static const  char* basic_status_list[] = {"ok", "failed"}; /* + not set */

/* -------------- shook status manager implementation ---------- */

#ifdef HAVE_SHOOK
static int shook_cl_cb(struct sm_instance *smi, const CL_REC_TYPE *logrec,
                       const entry_id_t *id, const attr_set_t *attrs,
                       attr_set_t *refreshed_attrs, bool *getit)
{
    /** TODO RBHv3 implement the following stuff */
    if (logrec->cr_type == CL_XATTR)
    {
        /* need to update status */
        *getit = true;
    }
    else if ( logrec->cr_type == CL_MTIME || logrec->cr_type == CL_TRUNC ||
              (logrec->cr_type == CL_CLOSE))
    {
        /** @TODO do like Lustre/HSM: 
         * if file is modified or truncated, need to check its status
         * (probably modified) EXCEPT if its status is already 'modified'
         */
    }
    else if (logrec->cr_type == CL_CTIME || (logrec->cr_type == CL_SETATTR))
    {
        /* in Lustre v2.O, changing trusted xattr generates CTIME/SATTR event */
        *getit = true;
    }


    if ( p_op->db_exists )
    {
        /* if the old name is a restripe file, update the status */
        if (!strncmp(RESTRIPE_TGT_PREFIX, ATTR( &p_op->db_attrs, name ),
                     strlen(RESTRIPE_TGT_PREFIX)))
        {
            p_op->fs_attr_need |= ATTR_MASK_status;
            DisplayLog( LVL_DEBUG, ENTRYPROC_TAG,
                        "Getstatus needed because entry was a restripe target");
        }
    }

    return 0;
}
#endif /* HAVE_SHOOK */

static status_manager_t basic_sm = {
    .name = "basic",
    .status_enum = basic_status_list,
    .status_count = BASIC_ST_COUNT
};

/* -------------- managing status managers ---------- */

/** TODO load status manager from dynamic module */
static status_manager_t *load_status_manager(const char *name)
{
    /** @TODO load from a dynamic module */
    if (!strcasecmp(name, "lhsm"))
        return &lhsm_sm;
    else if (!strcasecmp(name, "backup"))
        return &backup_sm;
    else if (!strcasecmp(name, "basic"))
        return &basic_sm;

    return NULL;
}

/** indicate if a status manager definition is already loaded */
static inline bool sm_loaded(const char *name, status_manager_t **sm_ptr)
{
    int i;
    for (i = 0; i < sm_count; i++)
    {
        if (!strcasecmp(sm_cache[i]->name, name))
        {
            *sm_ptr = sm_cache[i];
            return true;
        }
    }
    return false;
}

/** get a loaded status manager or allocate a new one */
static const status_manager_t *sm_get(const char *name)
{
    status_manager_t *tmp;

    if (sm_loaded(name, &tmp))
        return tmp;

    tmp = load_status_manager(name);
    if (tmp == NULL)
        return NULL;

    /* add it to the cache */
    sm_count++;
    sm_cache = realloc(sm_cache, sm_count * sizeof(status_manager_t*));
    if (sm_cache == NULL)
        return NULL;
    sm_cache[sm_count-1] = tmp;

    return tmp;
}


/** check if an instance of shared status manager exists */
static inline bool sm_instance_exists(const char *name, sm_instance_t **smi_ptr)
{
    int i;
    for (i = 0; i < sm_inst_count; i++)
    {
        if (!strcasecmp(sm_inst[i]->sm->name, name))
        {
            *smi_ptr = sm_inst[i];
            return true;
        }
    }
    return false;
}

/** create a status manager instance (if it does not already exist) */
sm_instance_t *create_sm_instance(const char *pol_name,const char *sm_name)
{
    sm_instance_t *smi = NULL;

    /* check that the status manager exists (load it if necessary) */
    const status_manager_t *sm = sm_get(sm_name);
    if (sm == NULL)
        return NULL;

    /* if it is shared, check if it is already instanciated */
    if ((sm->flags & SM_SHARED) && sm_instance_exists(sm_name, &smi))
        return smi;

    /* create an instance */
    smi = calloc(1, sizeof(sm_instance_t));
    if (smi == NULL)
        return NULL;

    smi->sm = sm;
    smi->smi_index = sm_inst_count;

    if (sm->flags & SM_SHARED)
    {
        /* If the status manager is shared between policies,
         * it just consists of the status manager name. */
        smi->instance_name = strdup(sm->name);
    }
    else /* private status manager (1 instance per policy) */
    {
        /* same as <policy name>\0 */
        smi->instance_name = malloc(strlen(pol_name)+1);
        if (smi->instance_name == NULL)
            goto free_smi;
        sprintf(smi->instance_name, "%s", pol_name);
    }
    /* <instance_name>_status */
    smi->db_field = malloc(strlen(smi->instance_name)+8);
    if (smi->db_field == NULL)
        goto free_str1;
    sprintf(smi->db_field, "%s_status", smi->instance_name);

    /* @TODO load its configuration */
    /* @TODO initialize it */

    /* add it the the list of SMIs */
    sm_inst_count++;
    sm_inst = realloc(sm_inst, sm_inst_count * sizeof(sm_instance_t*));
    if (sm_inst == NULL)
        goto free_str2;
    sm_inst[sm_inst_count-1] = smi;

    return smi;

free_str2:
    free(smi->db_field);
free_str1:
    free(smi->instance_name);
free_smi:
    free(smi);
    return NULL;
}

/** get the Nth status manager instance */
sm_instance_t * get_sm_instance(unsigned int n)
{
    if (n >= sm_inst_count)
        return NULL;
    else
        return sm_inst[n];
}

/** get the constant string that match the input string */
const char *get_status_str(const status_manager_t *sm, const char *in_str)
{
    int i;

    for (i = 0; i < sm->status_count; i++)
    {
        if (!strcmp(sm->status_enum[i], in_str))
            return sm->status_enum[i];
    }
    /* not found */
    return NULL;
}

/** return the list of allowed status for a status manager */
char *allowed_status_str(const status_manager_t *sm, char *buf, int sz)
{
    int i;
    char *cur = buf;
    buf[0] = '\0';

    rh_strncpy(cur, "\"\"(empty)", sz);
    cur += strlen(cur);

    for (i = 0; i < sm->status_count; i++)
    {
        if (!EMPTY_STRING(buf))
        {
            *cur = ',';
            cur++;
        }
        rh_strncpy(cur, sm->status_enum[i], sz - (ptrdiff_t)(cur - buf));
        cur += strlen(cur);
    }
    return buf;
}

int run_all_cl_cb(const CL_REC_TYPE *logrec, const entry_id_t *id,
                  const attr_set_t *attrs, attr_set_t *refreshed_attrs,
                  uint64_t *status_need)
{
    int rc, err_max = 0;
    int i = 0;
    sm_instance_t *smi;

    for (i = 0, smi = get_sm_instance(i); smi != NULL;
         i++, smi = get_sm_instance(i))
    {
        bool getstatus = false;

        if (smi->sm->changelog_cb == NULL)
            continue;

        rc = smi->sm->changelog_cb(smi, logrec, id, attrs, refreshed_attrs,
                                   &getstatus);
        if (err_max == 0 || rc > err_max)
            err_max = rc;

        if ((rc == 0) && getstatus)
        {
            *status_need |= SMI_MASK(i);
        }
    }
    return err_max;
}


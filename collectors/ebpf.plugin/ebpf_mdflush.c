// SPDX-License-Identifier: GPL-3.0-or-later

#include "ebpf.h"
#include "ebpf_mdflush.h"

struct config mdflush_config = { .first_section = NULL,
    .last_section = NULL,
    .mutex = NETDATA_MUTEX_INITIALIZER,
    .index = { .avl_tree = { .root = NULL, .compar = appconfig_section_compare },
        .rwlock = AVL_LOCK_INITIALIZER } };

#define MDFLUSH_MAP_COUNT 0
static ebpf_local_maps_t mdflush_maps[] = {
    {
        .name = "tbl_mdflush",
        .internal_input = 1024,
        .user_input = 0,
        .type = NETDATA_EBPF_MAP_STATIC,
        .map_fd = ND_EBPF_MAP_FD_NOT_INITIALIZED
    },
    /* end */
    {
        .name = NULL,
        .internal_input = 0,
        .user_input = 0,
        .type = NETDATA_EBPF_MAP_CONTROLLER,
        .map_fd = ND_EBPF_MAP_FD_NOT_INITIALIZED
    }
};

// store for "published" data from the reader thread, which the collector
// thread will write to netdata agent.
static avl_tree_lock mdflush_pub;

// tmp store for mdflush values we get from a per-CPU eBPF map.
static mdflush_ebpf_val_t *mdflush_ebpf_vals = NULL;

/**
 * MDflush Free
 *
 * Cleanup variables after child threads to stop
 *
 * @param ptr thread data.
 */
static void ebpf_mdflush_free(ebpf_module_t *em)
{
    freez(mdflush_ebpf_vals);
    pthread_mutex_lock(&ebpf_exit_cleanup);
    em->thread->enabled = NETDATA_THREAD_EBPF_STOPPED;
    pthread_mutex_unlock(&ebpf_exit_cleanup);
}

/**
 * MDflush exit
 *
 * Cancel thread and exit.
 *
 * @param ptr thread data.
 */
static void mdflush_exit(void *ptr)
{
    ebpf_module_t *em = (ebpf_module_t *)ptr;
    ebpf_mdflush_free(em);
}

/**
 * Compare mdflush values.
 *
 * @param a `netdata_mdflush_t *`.
 * @param b `netdata_mdflush_t *`.
 *
 * @return 0 if a==b, 1 if a>b, -1 if a<b.
*/
static int mdflush_val_cmp(void *a, void *b)
{
    netdata_mdflush_t *ptr1 = a;
    netdata_mdflush_t *ptr2 = b;

    if (ptr1->unit > ptr2->unit) {
        return 1;
    }
    else if (ptr1->unit < ptr2->unit) {
        return -1;
    }
    else {
        return 0;
    }
}

static void mdflush_read_count_map()
{
    int mapfd = mdflush_maps[MDFLUSH_MAP_COUNT].map_fd;
    mdflush_ebpf_key_t curr_key = (uint32_t)-1;
    mdflush_ebpf_key_t key = (uint32_t)-1;
    netdata_mdflush_t search_v;
    netdata_mdflush_t *v = NULL;

    while (bpf_map_get_next_key(mapfd, &curr_key, &key) == 0) {
        curr_key = key;

        // get val for this key.
        int test = bpf_map_lookup_elem(mapfd, &key, mdflush_ebpf_vals);
        if (unlikely(test < 0)) {
            continue;
        }

        // is this record saved yet?
        //
        // if not, make a new one, mark it as unsaved for now, and continue; we
        // will insert it at the end after all of its values are correctly set,
        // so that we can safely publish it to the collector within a single,
        // short locked operation.
        //
        // otherwise simply continue; we will only update the flush count,
        // which can be republished safely without a lock.
        //
        // NOTE: lock isn't strictly necessary for this initial search, as only
        // this thread does writing, but the AVL is using a read-write lock so
        // there is no congestion.
        bool v_is_new = false;
        search_v.unit = key;
        v = (netdata_mdflush_t *)avl_search_lock(
            &mdflush_pub,
            (avl_t *)&search_v
        );
        if (unlikely(v == NULL)) {
            // flush count can only be added reliably at a later time.
            // when they're added, only then will we AVL insert.
            v = callocz(1, sizeof(netdata_mdflush_t));
            v->unit = key;
            sprintf(v->disk_name, "md%u", key);
            v->dim_exists = false;

            v_is_new = true;
        }

        // we must add up count value for this record across all CPUs.
        uint64_t total_cnt = 0;
        int i;
        int end = (running_on_kernel < NETDATA_KERNEL_V4_15) ? 1 : ebpf_nprocs;
        for (i = 0; i < end; i++) {
            total_cnt += mdflush_ebpf_vals[i];
        }

        // can now safely publish count for existing records.
        v->cnt = total_cnt;

        // can now safely publish new record.
        if (v_is_new) {
            avl_t *check = avl_insert_lock(&mdflush_pub, (avl_t *)v);
            if (check != (avl_t *)v) {
                error("Internal error, cannot insert the AVL tree.");
            }
        }
    }
}

static void mdflush_create_charts(int update_every)
{
    ebpf_create_chart(
        "mdstat",
        "mdstat_flush",
        "MD flushes",
        "flushes",
        "flush (eBPF)",
        "md.flush",
        NETDATA_EBPF_CHART_TYPE_STACKED,
        NETDATA_CHART_PRIO_MDSTAT_FLUSH,
        NULL, NULL, 0, update_every,
        NETDATA_EBPF_MODULE_NAME_MDFLUSH
    );

    fflush(stdout);
}

// callback for avl tree traversal on `mdflush_pub`.
static int mdflush_write_dims(void *entry, void *data)
{
    UNUSED(data);

    netdata_mdflush_t *v = entry;

    // records get dynamically added in, so add the dim if we haven't yet.
    if (!v->dim_exists) {
        ebpf_write_global_dimension(
            v->disk_name, v->disk_name,
            ebpf_algorithms[NETDATA_EBPF_INCREMENTAL_IDX]
        );
        v->dim_exists = true;
    }

    write_chart_dimension(v->disk_name, v->cnt);

    return 1;
}

/**
* Main loop for this collector.
*/
static void mdflush_collector(ebpf_module_t *em)
{
    mdflush_ebpf_vals = callocz(ebpf_nprocs, sizeof(mdflush_ebpf_val_t));

    avl_init_lock(&mdflush_pub, mdflush_val_cmp);

    // create chart and static dims.
    pthread_mutex_lock(&lock);
    mdflush_create_charts(em->update_every);
    ebpf_update_stats(&plugin_statistics, em);
    pthread_mutex_unlock(&lock);

    // loop and read from published data until ebpf plugin is closed.
    heartbeat_t hb;
    heartbeat_init(&hb);
    usec_t step = em->update_every * USEC_PER_SEC;
    while (!ebpf_exit_plugin) {
        (void)heartbeat_next(&hb, step);
        if (ebpf_exit_plugin)
            break;

        mdflush_read_count_map();
        // write dims now for all hitherto discovered devices.
        write_begin_chart("mdstat", "mdstat_flush");
        avl_traverse_lock(&mdflush_pub, mdflush_write_dims, NULL);
        write_end_chart();

        pthread_mutex_unlock(&lock);
    }
}

/**
 * mdflush thread.
 *
 * @param ptr a `ebpf_module_t *`.
 * @return always NULL.
 */
void *ebpf_mdflush_thread(void *ptr)
{
    netdata_thread_cleanup_push(mdflush_exit, ptr);

    ebpf_module_t *em = (ebpf_module_t *)ptr;
    em->maps = mdflush_maps;

    char *md_flush_request = ebpf_find_symbol("md_flush_request");
    if (!md_flush_request) {
        em->thread->enabled = NETDATA_THREAD_EBPF_STOPPED;
        error("Cannot monitor MD devices, because md is not loaded.");
    }
    freez(md_flush_request);

    if (em->thread->enabled == NETDATA_THREAD_EBPF_STOPPED) {
        goto endmdflush;
    }

    em->probe_links = ebpf_load_program(ebpf_plugin_dir, em, running_on_kernel, isrh, &em->objects);
    if (!em->probe_links) {
        em->enabled = NETDATA_THREAD_EBPF_STOPPED;
        goto endmdflush;
    }

    mdflush_collector(em);

endmdflush:
    ebpf_update_disabled_plugin_stats(em);

    netdata_thread_cleanup_pop(1);

    return NULL;
}

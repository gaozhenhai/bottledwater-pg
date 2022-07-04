#include "io_util.h"
#include "protocol_server.h"
#include "oid2avro.h"
#include "error_policy.h"

#include "replication/logical.h"
#include "replication/output_plugin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/* Entry point when Postgres loads the plugin */
extern void _PG_init(void);
extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

static void output_avro_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init);
static void output_avro_shutdown(LogicalDecodingContext *ctx);
static void output_avro_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn);
static void output_avro_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void output_avro_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, Relation rel, ReorderBufferChange *change);

typedef struct {
    MemoryContext memctx; /* reset after every change event, to prevent leaks */
    avro_schema_t frame_schema;
    avro_value_iface_t *frame_iface;
    avro_value_t frame_value;
    schema_cache_t schema_cache;
    error_policy_t error_policy;
    char table_conf_flag;   /* '+' represents permission.'-' represents prohibition */
    int table_conf_num;
    char **table_conf_list;
} plugin_state;

void reset_frame(plugin_state *state);
int write_frame(LogicalDecodingContext *ctx, plugin_state *state);


void _PG_init() {
}

void _PG_output_plugin_init(OutputPluginCallbacks *cb) {
    AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);
    cb->startup_cb = output_avro_startup;
    cb->begin_cb = output_avro_begin_txn;
    cb->change_cb = output_avro_change;
    cb->commit_cb = output_avro_commit_txn;
    cb->shutdown_cb = output_avro_shutdown;
}

static void output_avro_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
        bool is_init) {
    ListCell *option;
    FILE * table_conf = NULL;
    char * path = NULL;           /*The path of table_list.conf*/
    plugin_state *state = palloc(sizeof(plugin_state));
    ctx->output_plugin_private = state;
    opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

    state->memctx = AllocSetContextCreate(ctx->context, "Avro decoder context",
            ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    state->table_conf_flag = '\0';        /* Initialization */
    state->table_conf_num = 0;        /* Initialization */
    state->table_conf_list = NULL;    /* Initialization */
    state->frame_schema = schema_for_frame();
    state->frame_iface = avro_generic_class_from_schema(state->frame_schema);
    avro_generic_value_new(state->frame_iface, &state->frame_value);
    state->schema_cache = schema_cache_new(ctx->context);

    foreach(option, ctx->output_plugin_options) {
        DefElem *elem = lfirst(option);
        Assert(elem->arg == NULL || IsA(elem->arg, String));

        if (strcmp(elem->defname, "error_policy") == 0) {
            if (elem->arg == NULL) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("No value specified for parameter \"%s\"",
                            elem->defname)));
            } else {
                state->error_policy = parse_error_policy(strVal(elem->arg));
            }
        } else if (strcmp(elem->defname, "table_list_path") == 0) {
            if (elem->arg == NULL) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("No value specified for parameter \"%s\"",
                            elem->defname)));
            } else {
                path = strdup(strVal(elem->arg));
            }
        } else {
            ereport(INFO, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Parameter \"%s\" = \"%s\" is unknown",
                        elem->defname,
                        elem->arg ? strVal(elem->arg) : "(null)")));
        }
    }

    /* read file "table_list.conf" and make space for state->table_conf_list */
    table_conf = fopen(path,"rb");        /* open file */
    if(table_conf != NULL){
        fscanf(table_conf,"%c",&(state->table_conf_flag));
        if(state->table_conf_flag == '+' || state->table_conf_flag == '-'){
            state->table_conf_list = palloc(100 * sizeof(char*));
            state->table_conf_list[state->table_conf_num] = palloc(100);
            while(fscanf(table_conf,"%s",state->table_conf_list[state->table_conf_num]) == 1){
                state->table_conf_num++;
                state->table_conf_list[state->table_conf_num] = palloc(100);
            }
        }else{
            ereport(LOG,(errmsg("'+' or '-' must be specified in the document 'table_list.conf'.")));
        }
        fclose(table_conf);
    }else{
        ereport(LOG,(errmsg("logdecoder open file %s failed.",path)));
    }
    if(path != NULL){
        free(path);
    }
    /* end read */
}

static void output_avro_shutdown(LogicalDecodingContext *ctx) {
    plugin_state *state = ctx->output_plugin_private;
    MemoryContextDelete(state->memctx);

    /* free space */
    if(state->table_conf_list){
        do{
            if(state->table_conf_list[state->table_conf_num]){
                pfree(state->table_conf_list[state->table_conf_num]);
            }
            state->table_conf_list[state->table_conf_num] = NULL;
        }while(state->table_conf_num--);
        pfree(state->table_conf_list);
        state->table_conf_list = NULL;
    }
    /* end free */

    schema_cache_free(state->schema_cache);
    avro_value_decref(&state->frame_value);
    avro_value_iface_decref(state->frame_iface);
    avro_schema_decref(state->frame_schema);
}

static void output_avro_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn) {
    plugin_state *state = ctx->output_plugin_private;
    MemoryContext oldctx = MemoryContextSwitchTo(state->memctx);
    reset_frame(state);

    if (update_frame_with_begin_txn(&state->frame_value, txn)) {
        elog(ERROR, "output_avro_begin_txn: Avro conversion failed: %s", avro_strerror());
    }
    if (write_frame(ctx, state)) {
        elog(ERROR, "output_avro_begin_txn: writing Avro binary failed: %s", avro_strerror());
    }

    MemoryContextSwitchTo(oldctx);
    MemoryContextReset(state->memctx);
}

static void output_avro_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
        XLogRecPtr commit_lsn) {
    plugin_state *state = ctx->output_plugin_private;
    MemoryContext oldctx = MemoryContextSwitchTo(state->memctx);
    reset_frame(state);

    if (update_frame_with_commit_txn(&state->frame_value, txn, commit_lsn)) {
        elog(ERROR, "output_avro_commit_txn: Avro conversion failed: %s", avro_strerror());
    }
    if (write_frame(ctx, state)) {
        elog(ERROR, "output_avro_commit_txn: writing Avro binary failed: %s", avro_strerror());
    }

    MemoryContextSwitchTo(oldctx);
    MemoryContextReset(state->memctx);
}

static void output_avro_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
        Relation rel, ReorderBufferChange *change) {
    int err = 0;
    char *relname;
    char *namespace_name;
    int i;
    char space_rel_name[100];
    HeapTuple oldtuple = NULL, newtuple = NULL;
    plugin_state *state = ctx->output_plugin_private;
    MemoryContext oldctx = MemoryContextSwitchTo(state->memctx);
    reset_frame(state);

    /* make up space_rel_name */
    relname = RelationGetRelationName(rel);
    namespace_name = get_namespace_name(RelationGetNamespace(rel));
    if(namespace_name != NULL){
        sprintf(space_rel_name,"%s.%s",namespace_name,relname);
        pfree(namespace_name);
    }else{
        sprintf(space_rel_name,"%s",relname);
    }
    /* end make up space_rel_name */

    /* search table */
    for(i=0;i < state->table_conf_num;i++){
        if(strcmp(space_rel_name,state->table_conf_list[i]) == 0){
            break;
        }
    }
    if(state->table_conf_flag == '+'){
        /* If space_rel_name is not in table_conf_list,then skip*/
        if(i == state->table_conf_num){
            return;
        }
    }else if(state->table_conf_flag == '-'){
        /* If space_rel_name is in table_conf_list,then skip*/
        if(i < state->table_conf_num){
            return;
        }
    }
    /* end search */

    switch (change->action) {
        case REORDER_BUFFER_CHANGE_INSERT:
            if (!change->data.tp.newtuple) {
                elog(ERROR, "output_avro_change: insert action without a tuple");
            }
            newtuple = &change->data.tp.newtuple->tuple;
            err = update_frame_with_insert(&state->frame_value, state->schema_cache, rel,
                    RelationGetDescr(rel), newtuple);
            break;

        case REORDER_BUFFER_CHANGE_UPDATE:
            if (!change->data.tp.newtuple) {
                elog(ERROR, "output_avro_change: update action without a tuple");
            }
            if (change->data.tp.oldtuple) {
                oldtuple = &change->data.tp.oldtuple->tuple;
            }
            newtuple = &change->data.tp.newtuple->tuple;
            err = update_frame_with_update(&state->frame_value, state->schema_cache, rel, oldtuple, newtuple);
            break;

        case REORDER_BUFFER_CHANGE_DELETE:
            if (change->data.tp.oldtuple) {
                oldtuple = &change->data.tp.oldtuple->tuple;
            }
            err = update_frame_with_delete(&state->frame_value, state->schema_cache, rel, oldtuple);
            break;

        default:
            elog(ERROR, "output_avro_change: unknown change action %d", change->action);
    }

    if (err) {
        elog(INFO, "Row conversion failed: %s", schema_debug_info(rel, NULL));
        error_policy_handle(state->error_policy, "output_avro_change: row conversion failed", avro_strerror());
        /* if handling the error didn't exit early, it should be safe to fall
         * through, because we'll just write the frame without the message that
         * failed (so potentially it'll be an empty frame)
         */
    }
    if (write_frame(ctx, state)) {
        error_policy_handle(state->error_policy, "output_avro_change: writing Avro binary failed", avro_strerror());
    }

    MemoryContextSwitchTo(oldctx);
    MemoryContextReset(state->memctx);
}

void reset_frame(plugin_state *state) {
    if (avro_value_reset(&state->frame_value)) {
        elog(ERROR, "Avro value reset failed: %s", avro_strerror());
    }
}

int write_frame(LogicalDecodingContext *ctx, plugin_state *state) {
    int err = 0;
    bytea *output = NULL;

    check(err, try_writing(&output, &write_avro_binary, &state->frame_value));

    OutputPluginPrepareWrite(ctx, true);
    appendBinaryStringInfo(ctx->out, VARDATA(output), VARSIZE(output) - VARHDRSZ);
    OutputPluginWrite(ctx, true);

    pfree(output);
    return err;
}

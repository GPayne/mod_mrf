/*
* An OnEarth module that serves tiles from an MRF
* Lucian Plesea
* (C) 2016-2019
*/

#include <ahtse.h>
#include "receive_context.h"

#include <algorithm>
#include <cmath>
#include <http_log.h>
#include <http_request.h>

#define CMD_FUNC (cmd_func)

using namespace std;

NS_AHTSE_USE

struct mrf_conf {
    // array of guard regexp, one of them has to match
    apr_array_header_t *arr_rxp;
    apr_array_header_t *source;
    vfile_t idx;

    // Forced mime-type, default is autodetected
    char *mime_type;
    // Full raster size in pixels
    sz size;
    // Page size in pixels
    sz pagesize;

    // Levels to skip at the top
    int skip_levels;
    int n_levels;
    rset *rsets;

    empty_conf_t empty;
    apr_off_t eoffset;

    // Turns the module functionality off
    int enabled;
    // If set, only secondary requests are allowed
    int indirect;

    // Used on remote data, how many times to try
    int tries;

    // ETag initializer
    apr_uint64_t seed;
    // Buffer for the emtpy tile etag
    char eETag[16];
};

extern module AP_MODULE_DECLARE_DATA mrf_module;

#if defined(APLOG_USE_MODULE)
APLOG_USE_MODULE(mrf);
#endif

static inline void *create_dir_config(apr_pool_t *p, char *dummy)
{
    mrf_conf *c =
        (mrf_conf *)apr_pcalloc(p, sizeof(mrf_conf));
    c->tries = 5;
    return c;
}

// Returns NULL if it worked as expected, returns a four integer value from "x y", "x y z" or "x y z c"
static char *get_xyzc_size(apr_pool_t *p, sz *size, 
    const char *value, const char*err_prefix)
{
    char *s;
    if (!value)
        return apr_psprintf(p, "%s directive missing", err_prefix);
    size->x = apr_strtoi64(value, &s, 0);
    size->y = apr_strtoi64(s, &s, 0);
    size->c = 3;
    size->z = 1;
    if (errno == 0 && *s) { // Read optional third and fourth integers
        size->z = apr_strtoi64(s, &s, 0);
        if (*s)
            size->c = apr_strtoi64(s, &s, 0);
    } // Raster size is 4 params max
    if (errno || *s)
        return apr_psprintf(p, "%s incorrect", err_prefix);
    return NULL;
}

static void mrf_init(apr_pool_t *p, mrf_conf *c) {
    rset level;
    level.w = static_cast<int>(1 + (c->size.x - 1) / c->pagesize.x);
    level.h = static_cast<int>(1 + (c->size.y - 1) / c->pagesize.y);
    level.offset = 0;
    // How many levels do we have
    c->n_levels = 2 + ilogb(max(level.h, level.w) - 1);
    c->rsets = (struct rset *)apr_pcalloc(p, sizeof(rset) * c->n_levels);

    // Populate rsets from the bottom, the way tile protcols count levels
    // These are MRF rsets, not all of them are visible
    struct rset *r = c->rsets + c->n_levels - 1;
    for (int i = 0; i < c->n_levels; i++) {
        *r-- = level;
        // Prepare for the next level, assuming powers of two
        // This is safe on all platforms that have large files (64bit signed offset)
        // It will start failing if the file offset is larger than 63bits
        // The c->size.z has to be first, to force the 64bit type
        level.offset += c->size.z * level.w * level.h * sizeof(range_t);
        level.w = 1 + (level.w - 1) / 2;
        level.h = 1 + (level.h - 1) / 2;
    }
    // MRF has one tile at the top
    ap_assert(c->rsets->h == 1 && c->rsets->w == 1);
}

// Allow for one or more RegExp guard
// If present, at least one of them has to match the URL
static const char *set_regexp(cmd_parms *cmd, mrf_conf *c, 
    const char *pattern)
{
    // char *err_message = NULL;
    if (c->arr_rxp == 0)
        c->arr_rxp = apr_array_make(cmd->pool, 2, sizeof(ap_regex_t *));
    ap_regex_t **m = (ap_regex_t **)apr_array_push(c->arr_rxp);
    *m = ap_pregcomp(cmd->pool, pattern, 0);
    return (nullptr != *m) ? nullptr : "Bad regular expression";
}

// Parse a comma separated list of sources, add the entries to the array arr
// Source may include offset and size, white space separated
static const char *parse_sources(cmd_parms *cmd, const char *src, 
    apr_array_header_t *arr, bool redir = false)
{
    apr_array_header_t *inputs = tokenize(cmd->temp_pool, src, ',');
    for (int i = 0; i < inputs->nelts; i++) {
        vfile_t *entry = &APR_ARRAY_PUSH(arr, vfile_t);
        memset(entry, 0, sizeof(vfile_t));
        char *input = APR_ARRAY_IDX(inputs, i, char *);

        char *fname = ap_getword_white_nc(arr->pool, &input);
        if (!fname)
            return "Missing source name";

        entry->name = fname;

        // See if there are more arguments, should be offset and size
        if (*input != 0) entry->range.offset = strtoull(input, &input, 0);
        if (*input != 0) entry->range.size = strtoull(input, &input, 0);
    }
    return nullptr;
}

static const char *parse_redirects(cmd_parms *cmd, const char *src,
    apr_array_header_t *arr)
{
    return parse_sources(cmd, src, arr, true);
}

static const char *mrf_file_set(cmd_parms *cmd, void *dconf, const char *arg)
{
    ap_assert(sizeof(apr_off_t) == 8);
    mrf_conf *c = (mrf_conf *)dconf;
    const char *err_message;
    apr_table_t *kvp = readAHTSEConfig(cmd->temp_pool, arg, &err_message);
    if (NULL == kvp) return err_message;

    // Got the parsed kvp table, parse the configuration items
    const char *line;
    char *err_prefix;

    line = apr_table_get(kvp, "Size");
    if (!line)
        return apr_psprintf(cmd->temp_pool, "%s Size directive is mandatory", arg);
    err_prefix = apr_psprintf(cmd->temp_pool, "%s Size", arg);
    err_message = get_xyzc_size(cmd->temp_pool, &(c->size), line, err_prefix);
    if (err_message) return err_message;

    // PageSize is optional, use reasonable defaults
    c->pagesize.x = c->pagesize.z = 512;
    c->pagesize.c = c->size.c;
    c->pagesize.z = 1;
    line = apr_table_get(kvp, "PageSize");
    if (line) {
        err_prefix = apr_psprintf(cmd->temp_pool, "%s PageSize", arg);
        err_message = get_xyzc_size(cmd->temp_pool, &(c->pagesize), line, err_prefix);
        if (err_message) return err_message;
    }
    if (c->pagesize.c != c->size.c || c->pagesize.z != 1)
        return apr_psprintf(cmd->temp_pool, "%s PageSize has invalid parameters", arg);

    // Initialize the run-time structures
    mrf_init(cmd->pool, c);

    if (!c->source)
        c->source = apr_array_make(cmd->pool, 1, sizeof(vfile_t));

    // The DataFile is alternative with Redirect
    if ((NULL != (line = apr_table_getm(cmd->temp_pool, kvp, "DataFile"))) &&
        (NULL != (line = parse_sources(cmd, line, c->source))))
        return line;

    // Old style redirects go at the end
    if ((NULL != (line = apr_table_getm(cmd->temp_pool, kvp, "Redirect"))) &&
        (NULL != (line = parse_redirects(cmd, line, c->source))))
        return line;

    line = apr_table_get(kvp, "RetryCount");
    c->tries = 1 + (line ? atoi(line) : 0);
    if ((c->tries < 1) || (c->tries > 100))
        return "Invalid RetryCount value, should be 0 to 99, defaults to 4";

    // Index file can also be provided, there could be a default
    line = apr_table_get(kvp, "IndexFile");
    c->idx.name = apr_pstrdup(cmd->pool, line);

    // Mime type is autodetected if not provided
    line = apr_table_get(kvp, "MimeType");
    if (line)
        c->mime_type = apr_pstrdup(cmd->pool, line);

    // Skip levels, from the top of the MRF
    line = apr_table_get(kvp, "SkippedLevels");
    if (line)
        c->skip_levels = atoi(line);

    // If an emtpy tile is not provided, it falls through, which results in a 404 error
    // If provided, it has an optional size and offset followed by file name which 
    // defaults to datafile read the empty tile
    // Default file name is the name of the first data file, if provided
    const char *datafname = NULL;
    for (int i = 0; i < c->source->nelts; i++)
        if (NULL != (datafname = APR_ARRAY_IDX(c->source, i, vfile_t).name))
            break;

    const char *efname = datafname;
    line = apr_table_get(kvp, "EmptyTile");
    if (line) {
        char *last;
        // Try to read a figure first
        c->empty.empty.size = (int)apr_strtoi64(line, &last, 0);

        // If that worked, try to get an offset too
        if (last != line)
            apr_strtoff(&(c->eoffset), last, &last, 0);

        // If there is anything left
        while (*last && isspace(*last)) last++;
        if (*last != 0)
            efname = last;
    }

    // If we're provided a file name or a size, pre-read the empty tile in the
    if (efname && 
        (datafname == NULL || apr_strnatcmp(datafname, efname) || c->empty.empty.size))
    {
        apr_file_t *efile;
        apr_off_t offset = c->eoffset;
        apr_status_t stat;

        // Use the temp pool for the file open, it will close it for us
        if (!c->empty.empty.size) { // Don't know the size, get it from the file
            apr_finfo_t finfo;
            stat = apr_stat(&finfo, efname, APR_FINFO_CSIZE, cmd->temp_pool);
            if (APR_SUCCESS != stat)
                return apr_psprintf(cmd->pool, "Can't stat %s %pm", efname, &stat);
            c->empty.empty.size = (int)finfo.csize;
        }

        stat = apr_file_open(&efile, efname, APR_FOPEN_READ | APR_FOPEN_BINARY, 0, cmd->temp_pool);
        if (APR_SUCCESS != stat)
            return apr_psprintf(cmd->pool, "Can't open empty file %s, loaded from %s: %pm",
            efname, arg, &stat);
        c->empty.empty.buffer = reinterpret_cast<char *>(apr_palloc(cmd->pool, 
            static_cast<apr_size_t>(c->empty.empty.size)));
        stat = apr_file_seek(efile, APR_SET, &offset);
        if (APR_SUCCESS != stat)
            return apr_psprintf(cmd->pool, "Can't seek empty tile %s: %pm", efname, &stat);
        apr_size_t size = c->empty.empty.size;
        stat = apr_file_read(efile, c->empty.empty.buffer, &size);
        if (APR_SUCCESS != stat)
            return apr_psprintf(cmd->pool, "Can't read from %s, loaded from %s: %pm",
            efname, arg, &stat);
        apr_file_close(efile);
    }

    line = apr_table_get(kvp, "ETagSeed");
    // Ignore the flag
    int flag;
    c->seed = line ? base32decode(line, &flag) : 0;
    // Set the missing tile etag, with the extra bit set
    tobase32(c->seed, c->eETag, 1);

    // Set the index file name based on the first data file, if there is only one
    if (!c->idx.name) {
        if (!datafname)
            return "Missing IndexFile or DataFile directive";
        c->idx.name = apr_pstrdup(cmd->pool, datafname);
        char *last;
        char *token = apr_strtok(c->idx.name, ".", &last); // strtok destroys the idxfile
        while (*last != 0 && token != NULL)
            token = apr_strtok(NULL, ".", &last);
        memcpy(c->idx.name, datafname, strlen(datafname)); // Get a new copy
        if (token != NULL && strlen(token) == 3)
            memcpy(token, "idx", 3);
    }

    c->enabled = 1;
    return NULL;
}

// An open file handle and the matching file name, to be used as a note
struct file_note {
    const char *name;
    apr_file_t *pfh;
};

static const apr_int32_t open_flags = APR_FOPEN_READ | APR_FOPEN_BINARY | APR_FOPEN_LARGEFILE;

/*
 * Open or retrieve an connection cached file.
 */
static apr_status_t open_connection_file(request_rec *r, apr_file_t **ppfh, 
    const vfile_t &src, const char *note_name, apr_int32_t flags = open_flags)
{
    apr_table_t *conn_notes = r->connection->notes;

    // Try to pick it up from the connection notes
    file_note *fn = (file_note *) apr_table_get(conn_notes, note_name);
    if ((fn != NULL) && !apr_strnatcmp(src.name, fn->name)) { // Match, set file and return
        *ppfh = fn->pfh;
        return APR_SUCCESS;
    }

    // Use the connection pool for the note and file, to ensure it gets closed with the connection
    apr_pool_t *pool = r->connection->pool;

    if (fn != NULL) { // We have a file note but it is not the right file
        apr_table_unset(conn_notes, note_name); // Unhook the existing note
        apr_file_close(fn->pfh); // Close the existing file
    }
    else { // no previous note, allocate a clean one
        fn = (file_note *)apr_palloc(pool, sizeof(file_note));
    }

    apr_status_t stat = apr_file_open(ppfh, src.name, flags, 0, pool);
    if (stat != APR_SUCCESS) 
        return stat;

    // Fill the note and hook it up, then return
    fn->pfh = *ppfh;
    fn->name = apr_pstrdup(pool, src.name);
    apr_table_setn(conn_notes, note_name, (const char *) fn);
    return APR_SUCCESS;
}

// Open data file optimized for random access if possible
static apr_status_t open_data_file(request_rec *r, apr_file_t **ppfh, 
    const vfile_t &src)
{
    static const char data_note_name[] = "MRF_DATA_FILE";

#if defined(APR_FOPEN_RANDOM)
    // apr has portable support for random access to files
    return open_connection_file(r, ppfh, src, open_flags | APR_FOPEN_RANDOM, 
        data_note_name);
#else

    apr_status_t stat = 
        open_connection_file(r, ppfh, src, data_note_name, open_flags);

#if !defined(POSIX_FADV_RANDOM)
    return stat;

#else // last chance, turn random flag on if supported
    apr_os_file_t fd;
    if (APR_SUCCESS == apr_os_file_get(&fd, *ppfh))
        posix_fadvise(static_cast<int>(fd), 0, 0, POSIX_FADV_RANDOM);
    return stat;
#endif
#endif // APR_FOPEN_RANDOM
}

// Quiet error
#define REQ_ERR_IF(X) if (X) {\
    return HTTP_BAD_REQUEST; \
}

// Logged error
#define SERR_IF(X, msg) if (X) { \
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s", msg);\
    return HTTP_INTERNAL_SERVER_ERROR; \
}

// Return the first source which contains the index, adjusts the index offset if necessary
static const vfile_t *get_source(const apr_array_header_t *sources, range_t *index) {
    for (int i = 0; i < sources->nelts; i++) {
        vfile_t *source = &APR_ARRAY_IDX(sources, i, vfile_t);
        if ((source->range.offset == 0 && source->range.size == 0)
            || (index->offset >= source->range.offset 
                && (source->range.size == 0
                    || index->offset - source->range.offset + index->size <= source->range.size)))
        {
            index->offset -= source->range.offset;
            return source;
        }
    }
    return NULL;
}

static const char *read_index(request_rec *r, range_t *idx, apr_off_t offset) {
    auto  cfg = get_conf<mrf_conf>(r, &mrf_module);
    apr_file_t *idxf;

    if (open_connection_file(r, &idxf, cfg->idx, "MRF_INDEX_FILE"))
        return apr_psprintf(r->pool,
            "Can't open index %s", cfg->idx.name);

    apr_size_t read_size = sizeof(range_t);
    if (apr_file_seek(idxf, APR_SET, &offset)
        || apr_file_read(idxf, idx, &read_size)
        || read_size != sizeof(range_t))
        return apr_psprintf(r->pool,
            "%s : Read error", cfg->idx.name);

    idx->offset = be64toh(idx->offset);
    idx->size = be64toh(idx->size);
    return nullptr;
}

static int handler(request_rec *r)
{
    // Only get and no arguments
    if (r->args || r->method_number != M_GET)
        return DECLINED;

    auto cfg = get_conf<mrf_conf>(r, &mrf_module);
    if (!cfg->enabled || (cfg->indirect && !r->main) || !requestMatches(r, cfg->arr_rxp))
        return DECLINED;

    apr_array_header_t *tokens = tokenize(r->pool, r->uri, '/');
    if (tokens->nelts < 3)
        return DECLINED; // At least Level Row Column

    // Use a xyzc structure, with c being the level
    // Input order is M/Level/Row/Column, with M being optional
    sz tile;
    memset(&tile, 0, sizeof(tile));

    // Need at least three numerical arguments
    tile.x = apr_atoi64(*(char **)apr_array_pop(tokens)); REQ_ERR_IF(errno);
    tile.y = apr_atoi64(*(char **)apr_array_pop(tokens)); REQ_ERR_IF(errno);
    tile.l = apr_atoi64(*(char **)apr_array_pop(tokens)); REQ_ERR_IF(errno);

    // We can ignore the error on this one, defaults to zero
    // The parameter before the level can't start with a digit for an extra-dimensional MRF
    if (cfg->size.z != 1 && tokens->nelts)
        tile.z = apr_atoi64(*(char **)apr_array_pop(tokens));

    // Don't allow access to levels less than zero, send the empty tile instead
    if (tile.l < 0)
        return sendEmptyTile(r, cfg->empty);

    tile.l += cfg->skip_levels;
    // Check for bad requests, outside of the defined bounds
    REQ_ERR_IF(tile.l >= cfg->n_levels);
    rset *level = cfg->rsets + tile.l;
    REQ_ERR_IF(tile.x >= level->w || tile.y >= level->h);

    // Offset of the index entry for this tile
    apr_off_t tidx_offset = level->offset +
        sizeof(range_t) * (tile.x + level->w * (tile.z * level->h + tile.y));

    range_t index;
    const char *message;
    SERR_IF((message = read_index(r, &index, tidx_offset)),
        message);

    // MRF index record is in network order
    if (index.size < 4) // Need at least four bytes for signature check
        return sendEmptyTile(r, cfg->empty);

    if (MAX_TILE_SIZE < index.size) { // Tile is too large, log and send error code
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "Tile too large in %s", 
            cfg->idx.name);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    // Check for conditional ETag here, no need to get the data
    char ETag[16];
    // Try to distribute the bits a bit to generate an ETag
    tobase32(cfg->seed ^ (index.size << 40) ^ index.offset, ETag);
    if (etagMatches(r, ETag)) {
        apr_table_set(r->headers_out, "ETag", ETag);
        return HTTP_NOT_MODIFIED;
    }

    // Now for the data part
    const vfile_t *src = get_source(cfg->source, &index);
    const char *name = (src && src->name) ? src->name : nullptr;
    if (!name)
        SERR_IF(true, apr_psprintf(r->pool, "No data file configured for %s", r->uri));

    bool redirect = (strlen(name) > 2 && name[0] == ':' && name[1] == '/');

    apr_uint32_t *buffer = static_cast<apr_uint32_t *>(
        apr_palloc(r->pool, static_cast<apr_size_t>(index.size)));
    SERR_IF(!buffer,
        "Memory allocation error in mod_mrf");

    if (redirect) {
        const char *new_uri = name + 2; // Skip the :/
        // TODO: S3 authorized requests
        ap_filter_rec_t *receive_filter = ap_get_output_filter_handle("Receive");
        SERR_IF(!receive_filter, "Using redirect requires mod_receive");

        // Get a buffer for the received image
        receive_ctx rctx;
        rctx.buffer = reinterpret_cast<char *>(buffer);
        rctx.maxsize = static_cast<int>(index.size);
        rctx.size = 0;

        // Data file is on a remote site a range request redirect with a range header
        char *Range = apr_psprintf(r->pool, "bytes=%" APR_UINT64_T_FMT "-%" APR_UINT64_T_FMT,
            index.offset, index.offset + index.size);

        // S3 may return less than requested, so we retry the request a couple of times
        int tries = cfg->tries;
        apr_time_t now = apr_time_now();
        do {
            request_rec *sr = ap_sub_req_lookup_uri(new_uri, r, r->output_filters);
            apr_table_setn(sr->headers_in, "Range", Range);
            ap_filter_t *rf = ap_add_output_filter_handle(receive_filter, &rctx, 
                sr, sr->connection);
            int status = ap_run_sub_req(sr);
            ap_remove_output_filter(rf);
            ap_destroy_sub_req(sr);

            if ((status != APR_SUCCESS || sr->status != HTTP_PARTIAL_CONTENT
                || rctx.size != static_cast<int>(index.size))
            && (0 == tries--))
            { // Abort here
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, 
                    "Can't fetch data from %s, took us %" APR_TIME_T_FMT,
                    new_uri, apr_time_now() - now);
                return HTTP_SERVICE_UNAVAILABLE;
            }
        } while (rctx.size != static_cast<int>(index.size));
    }
    else
    { // Read from a local file
        apr_file_t *dataf;
        SERR_IF(open_data_file(r, &dataf, *src),
            apr_psprintf(r->pool, "Can't open %s", name));

        // We got the tile index, and is not empty
        SERR_IF(apr_file_seek(dataf, APR_SET, (apr_off_t *)&index.offset),
            apr_psprintf(r->pool, "Seek error in %s", name));

        apr_size_t read_size = static_cast<apr_size_t>(index.size);
        SERR_IF(apr_file_read(dataf, buffer, &read_size) || read_size != index.size,
            apr_psprintf(r->pool, "Can't read from %s", name));
    }

    // Looks fine, set the outgoing etag and then the image
    apr_table_set(r->headers_out, "ETag", ETag);
    storage_manager temp = { (char *)buffer, static_cast<int>(index.size) };
    return sendImage(r, temp);
}

static const command_rec mrf_cmds[] =
{
    AP_INIT_FLAG(
    "MRF",
    CMD_FUNC ap_set_flag_slot,
    (void *)APR_OFFSETOF(mrf_conf, enabled),
    ACCESS_CONF,
    "mod_mrf enable, defaults to on if configuration is provided"
    ),

    AP_INIT_FLAG(
    "MRF_Indirect",
    CMD_FUNC ap_set_flag_slot,
    (void *)APR_OFFSETOF(mrf_conf, indirect),
    ACCESS_CONF,
    "If set, this configuration only responds to subrequests"
    ),

    AP_INIT_TAKE1(
    "MRF_RegExp",
    (cmd_func)set_regexp,
    0, // Self-pass argument
    ACCESS_CONF, // availability
    "Regular expression that the URL has to match.  At least one is required."
    ),

    AP_INIT_TAKE1(
    "MRF_ConfigurationFile",
    CMD_FUNC mrf_file_set, // Callback
    0, // Self-pass argument
    ACCESS_CONF, // availability
    "The configuration file for this module"
    ),

    { NULL }
};

static void mrf_register_hooks(apr_pool_t *p) {
    ap_hook_handler(handler, NULL, NULL, APR_HOOK_FIRST);
}

module AP_MODULE_DECLARE_DATA mrf_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_config,
    0, // No dir_merge
    0, // No server_config
    0, // No server_merge
    mrf_cmds, // configuration directives
    mrf_register_hooks // processing hooks
};

/* ============================================================================
 *  filesystem.h — In-memory hierarchical FS + File Manager (Titan v3)
 * ==========================================================================*/
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

typedef unsigned char  fs_u8;
typedef unsigned int   fs_u32;
typedef int            fs_bool;
#define FS_TRUE  1
#define FS_FALSE 0

#define FS_MAX_NAME     24
#define FS_MAX_CHILDREN 20
#define FS_MAX_NODES    80
#define FS_MAX_CONTENT  1536

typedef enum { FS_DIR = 0, FS_FILE = 1 } fs_type_t;

typedef struct fs_node {
    char            name[FS_MAX_NAME];
    fs_type_t       type;
    struct fs_node *parent;
    struct fs_node *kids[FS_MAX_CHILDREN];
    int             nkids;
    char           *data;
    fs_u32          dlen;
    fs_u32          dcap;
} fs_node_t;

void        fs_init(void);
fs_node_t  *fs_root(void);
fs_node_t  *fs_mkdir(fs_node_t *parent, const char *name);
fs_node_t  *fs_mkfile(fs_node_t *parent, const char *name);
fs_node_t  *fs_child(fs_node_t *parent, const char *name);
fs_bool     fs_delete(fs_node_t *node);
fs_bool     fs_write(fs_node_t *file, const char *text, fs_u32 len);
const char *fs_read(fs_node_t *file, fs_u32 *out_len);
void        fs_path(fs_node_t *node, char *buf, int buflen);

/* ---- File Manager ---- */
#define FM_ROWS 11

typedef struct {
    fs_node_t *dir;
    int        sel;          /* selected index, -1 = none */
    int        scroll;
    fs_bool    naming;       /* entering new name */
    fs_bool    name_is_dir;
    char       namebuf[FS_MAX_NAME];
    int        namelen;
    char       status[72];
} fm_state_t;

extern fm_state_t g_fm;

void fm_init(void);
void fm_reset_view(void);
void fm_key(char c);
void fm_click(int lx, int ly);   /* coords relative to content area */
void fm_go_up(void);
void fm_open_sel(void);
void fm_begin_new(fs_bool is_dir);
void fm_commit_new(void);

#endif

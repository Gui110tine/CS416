/*
  Simple File System

  This code is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.

*/

#include "params.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <sys/mman.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"
#include "block.h"

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//

/***** struct stat *****/
 //dev_t     st_dev;         /* ID of device containing file, ignored */
 //ino_t     st_ino;         /* inode number, ignored */
 //mode_t    st_mode;        /* protection */
 //nlink_t   st_nlink;       /* number of hard links */
 //uid_t     st_uid;         /* user ID of owner */
 //gid_t     st_gid;         /* group ID of owner */
 //dev_t     st_rdev;        /* device ID (if special file), ignored */
 //off_t     st_size;        /* total size, in bytes */
 //blksize_t st_blksize;     /* blocksize for filesystem I/O, ignored */
 //blkcnt_t  st_blocks;      /* number of 512B blocks allocated */

/***** MACROS *****/
#define MAX_NODES 128
#define MAX_LINK 128
#define NUM_BLOCKS 32768
#define ALLOCD 1
#define FREE 0
#define KEY 0xA7C4144BEF51B825

/***** GLOBALS *****/	// all these need to be kept on disk
int64_t         key_val;
inode_t 	inode_table[MAX_NODES];
path_map_t	name_table[MAX_LINK];
file_entry_t	open_files[MAX_LINK];
short		disk_blocks[NUM_BLOCKS];

int root_inode;
int root_block;

int system_block;
int cwd_inode;	// holds inode of the current working directory

int get_inode() {
	int i, j;
	for(i = 0; i < MAX_NODES; i++) {
		if(inode_table[i].avail == FREE) {
			inode_table[i].avail = ALLOCD;
			for(j = 0; j < 12; j++) {
				inode_table[i].blocks[j] = -1;
			}
			return i;
		}
	}	
	return -1;
}

void free_inode(int st_ino) {
	inode_table[st_ino].stat.st_nlink--;
	if(inode_table[st_ino].stat.st_nlink > 0) {
		return;
	}

	memset( &(inode_table[st_ino].stat), 0, sizeof(struct stat) );
	int i, j;
	
	char buf1[BLOCK_SIZE];
	char buf2[BLOCK_SIZE];
	short * buf_mask1 = (short *)buf1;
	short * buf_mask2 = (short *)buf2;

	if (inode_table[st_ino].blocks[11] > 0) {
		block_read(inode_table[st_ino].blocks[11], (void *)buf1);
		for (i = 0; i < 10; i++) {
			if (buf_mask1[i] > 0) {
				block_read(buf_mask1[i], buf2);
				for (j = 0; j < 100; j++) {
					if (buf_mask2[j] > 0) {
						disk_blocks[buf_mask2[j]] = FREE;
					} else { break; }
				}
				disk_blocks[buf_mask1[i]] = FREE;
			} else { break; }
		}
		disk_blocks[inode_table[st_ino].blocks[11]] = FREE;

	}

	if (inode_table[st_ino].blocks[10] > 0) {
		block_read(inode_table[st_ino].blocks[10], (void *)buf1);
		for (i = 0; i < 100; i++) {
			if (buf_mask1[i] > 0) {
				disk_blocks[buf_mask1[i]] = FREE;
			} else { break; }
		}
		disk_blocks[inode_table[st_ino].blocks[10]] = FREE;
	}

	for(i = 0; i < 10; i++) {
		if (inode_table[st_ino].blocks > 0) {
			disk_blocks[inode_table[st_ino].blocks[i]] = FREE;
                	inode_table[st_ino].blocks[i] = -1;
		} else { break; }
        }

	inode_table[st_ino].avail = FREE;
	return;
}

int get_mapping() {
	int i;
        for(i = 0; i < MAX_LINK; i++) {
                if(name_table[i].avail == FREE) {
                        name_table[i].avail = ALLOCD;
                        return i;
                }
        }       
        return -1;	
}

void free_mapping(int mapping) {
	name_table[mapping].avail = FREE;
	name_table[mapping].st_ino = -1;
	memset( name_table[mapping].path, 0, MAX_PATH_LEN);
	return;
}

short get_block() {
	short i;
	for(i = 0; i < NUM_BLOCKS; i++) {
		if(disk_blocks[i] == FREE) {
			disk_blocks[i] = ALLOCD;
			return i;
		}
	}
	return -1;
}

short get_block_index(inode_t * inode, short block_num) {
	
	char buf[BLOCK_SIZE];
	short sub_index;

	if (block_num < 10) {
		return inode->blocks[block_num];
	} else if (block_num < 110) {
		block_read(inode->blocks[10], (void *)buf);
		return ((short *)buf)[block_num-10];
	} else if (block_num < 1110) {
		block_read(inode->blocks[11], buf);
		sub_index = ((short *)buf)[(block_num-110)/100];
		block_read(sub_index, (void *)buf);
		return ((short *)buf)[(block_num-110)%100];
	}

	return -1;
}

void set_block_index(inode_t * inode, short block_num, short index) {

        char buf[BLOCK_SIZE];
	short sub_index,i;

        if (block_num < 10) {
                inode->blocks[block_num] = index;

        } else if (block_num < 110) {
		if (inode->blocks[10] < 0) {
			inode->blocks[10] = get_block();
			block_read(inode->blocks[10], (void *)buf);
			for (i = 0; i < 100; i++) {
				((short *)buf)[i] = -1;
			}
		} else {
                	block_read(inode->blocks[10], (void *)buf);
		}
                ((short *)buf)[block_num-10] = index;
		block_write(inode->blocks[10], buf);

        } else if (block_num <= 1110) {
                if (inode->blocks[11] < 0) {
                        inode->blocks[11] = get_block();
                        for (i = 0; i < 10; i++) {
                                ((short *)buf)[i] = -1;
                        }
			block_write(inode->blocks[11], buf);
		} else {
                	block_read(inode->blocks[11], buf);
		}

                if (((short *)buf)[(block_num-110)/100] < 0) {
                        sub_index = ((short *)buf)[(block_num-110)/100] = get_block();
			block_write(inode->blocks[11], buf);
                        for (i = 0; i < 100; i++) {
                                ((short *)buf)[i] = -1;
                        }
		} else {
			sub_index = ((short *)buf)[(block_num-110)/100];
               		block_read(sub_index, buf);
		}

                ((short *)buf)[(block_num-110)%100] = index;
		block_write(sub_index, buf);
        }

}

void free_block(int i) {
	disk_blocks[i] = FREE;
	return;
}

int get_fh() {
	int i;
	for(i = 0; i < MAX_LINK; i++) {
		if(open_files[i].st_ino < 0) {
			return i;
		}
	}
	return -1;
}

void free_fh(int fh) {
	open_files[fh].st_ino = -1;
	open_files[fh].pos = 0;
	open_files[fh].refcnt = 0;
	return;
}

int path_lookup(const char *path) {
	int i;
	for(i = 0; i < MAX_LINK; i++) {
		if( strcmp(name_table[i].path, path) == 0 ) {
			return i;
		}
	}
	return -1;
}

int get_cwd_path(const char *path, char *buf) {
	if(strcmp(path, "/") == 0) {
		sprintf(buf, "/");
		return 0;
	}

	int i, pos = -1;
	for(i = 0; i < strlen(path); i++) {
		if(path[i] == '/') {
			pos = i;
		}
	}
	if(pos < 0) {
		return -1;
	} else if(pos == 0) {
		sprintf(buf, "/");
                return 0;
	}

	strncpy(buf, path, pos);
	return 0;
}

int get_name(const char *path, char *buf) {
        if(strcmp(path, "/") == 0) {
                sprintf(buf, "/");
                return 0;
        }

        int i, pos = -1;
        for(i = 0; i < strlen(path); i++) {
                if(path[i] == '/') {
                        pos = i;
                }
        }
        if(pos < 0) {
                return -1;
        }

        strncpy(buf, path+pos+1, strlen(path)-pos);
}

void removeSubstring(char *s,const char *toremove) {
  	while( s = strstr(s, toremove) ) {
    		memmove( s, s+strlen(toremove), 1+strlen(s+strlen(toremove)) );
	}
	return;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
void *sfs_init(struct fuse_conn_info *conn)
{
	fprintf(stderr, "in bb-init\n");
    	log_msg("\nsfs_init()\n");
    	log_conn(conn);
    	log_fuse_context(fuse_get_context());

	/* set diskfile based on sfs_state */
	disk_open((SFS_DATA)->diskfile);

	size_t reserve = sizeof(int64_t)+sizeof(inode_table)+sizeof(name_table)+sizeof(disk_blocks)+sizeof(open_files)+2*sizeof(int);
        int reserve_blocks = (reserve + BLOCK_SIZE)/BLOCK_SIZE;

	int i;
	int new_mount = 1;                      // 1 if file never mounted, 0 if re-mounting previous file system
        void *buf = malloc(BLOCK_SIZE);
        int64_t key_val = 0;
        for(i = 0; i < NUM_BLOCKS; i++) {
                block_read(NUM_BLOCKS-i, buf);
                if( *((int64_t*)buf) == KEY) {
                        new_mount = 0;
                        system_block = NUM_BLOCKS-i;
                        break;  
                }       
        }
        free(buf);	

	if(i == NUM_BLOCKS) {	// key not found
		system_block = NUM_BLOCKS - reserve_blocks;
	} else {		// key found
		system_block = NUM_BLOCKS - i;
	}

	if(new_mount == 0) {
        	buf = malloc(reserve_blocks*BLOCK_SIZE);
        	char *pos = (char*)buf;
	
		int i;
                for(i = 0; i < reserve_blocks; i++) {
                        block_read(system_block+i, buf+i*BLOCK_SIZE);
                }

        	memcpy(&key_val, pos, sizeof(int64_t));
        	pos += sizeof(int64_t);

        	memcpy(inode_table, pos, sizeof(inode_table));
        	pos += sizeof(inode_table);

	        memcpy(name_table, pos, sizeof(name_table));
       		 pos += sizeof(name_table);

        	memcpy(disk_blocks, pos, sizeof(disk_blocks));
        	pos += sizeof(disk_blocks);

        	memcpy(open_files, pos, sizeof(open_files));
        	pos += sizeof(open_files);

        	memcpy(&root_inode, pos, sizeof(int));
        	pos += sizeof(int);

        	memcpy(&root_block, pos, sizeof(int));

        	free(buf);	
		cwd_inode = root_inode;
		return SFS_DATA;
	}

	/* resize file to 16MB   */
	truncate((SFS_DATA)->diskfile, NUM_BLOCKS*BLOCK_SIZE);	

	/* zero out all tables (add check for pre existing file system) */
	memset(inode_table, 0, sizeof(inode_t)*MAX_NODES);
	memset(name_table,  0, sizeof(path_map_t)*MAX_LINK);
	memset(disk_blocks, FREE, sizeof(short)*NUM_BLOCKS);
	memset(open_files, 0, sizeof(file_entry_t)*MAX_LINK);

	/* enter root directory into inode table */
	root_inode = get_inode();
	inode_table[root_inode].stat.st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        inode_table[root_inode].stat.st_nlink = 2;
        inode_table[root_inode].stat.st_uid = getuid();
        inode_table[root_inode].stat.st_gid = getgid();
        inode_table[root_inode].stat.st_size = BLOCK_SIZE;
        inode_table[root_inode].stat.st_blocks = 1;
	inode_table[root_inode].stat.st_atime = time(NULL);
        inode_table[root_inode].stat.st_mtime = time(NULL);
        inode_table[root_inode].stat.st_ctime = time(NULL);
	
	for(i = 0; i < 12; i++) {
		inode_table[root_inode].blocks[i] = -1;
	}

	for(i = 0; i < MAX_LINK; i++) {
		open_files[i].st_ino = -1;
	}

	/* enter root directory into name table */
	int root_mapping = get_mapping();
	sprintf(name_table[root_mapping].path, "/");
	name_table[root_mapping].st_ino = root_inode;

	/* allocate disk block for root */
	root_block = get_block();
	set_block_index(&(inode_table[root_inode]), 0, root_block);
	// inode_table[root_inode].blocks[0] = root_block;

	/* 
 	 * add . and .. as entries to root the syntax for directory data will
 	 * be <file name>/<inode #>/<other file name>/<other inode #>/...
 	 */
	buf = malloc(BLOCK_SIZE);
	memset(buf, 0, BLOCK_SIZE);
	block_write(0, buf);
	
	block_write(0, "./0/../0");
	inode_table[root_inode].stat.st_size = 9;

    	return SFS_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void sfs_destroy(void *userdata)
{	
	/* write all tables to persistent memory */
	
	key_val = KEY;
        size_t reserve = sizeof(int64_t)+sizeof(inode_table)+sizeof(name_table)+sizeof(disk_blocks)+sizeof(open_files)+2*sizeof(int); 
	int reserve_blocks = (reserve + BLOCK_SIZE)/BLOCK_SIZE;
	void *buf = malloc(reserve_blocks*BLOCK_SIZE);
	char *pos = (char*)buf;

        memcpy(pos, &key_val, sizeof(int64_t));
        pos += sizeof(int64_t);
        
        memcpy(pos, inode_table, sizeof(inode_table));
        pos += sizeof(inode_table);
             
        memcpy(pos, name_table, sizeof(name_table));
        pos += sizeof(name_table); 
    
        memcpy(pos, disk_blocks, sizeof(disk_blocks));
        pos += sizeof(disk_blocks);
    
        memcpy(pos, open_files, sizeof(open_files));
        pos += sizeof(open_files);
    
        memcpy(pos, &root_inode, sizeof(int));
        pos += sizeof(int);

        memcpy(pos, &root_block, sizeof(int));

	int i;
	for(i = 0; i < reserve_blocks; i++) {
		block_write(system_block+i, buf+i*BLOCK_SIZE);
	}

	free(buf);
	
	disk_close();

	log_msg("\nsfs_destroy(userdata=0x%08x)\n", userdata);
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int sfs_getattr(const char *path, struct stat *statbuf)
{
	int retstat = 0;
    	char fpath[PATH_MAX];
    	log_msg("\nsfs_getattr(path=\"%s\", statbuf=0x%08x)\n", path, statbuf);	// this segfaults for some reason
	
	int mapping = path_lookup(path);
	
	if(mapping < 0) {	// path not found
		memset(statbuf, 0, sizeof(struct stat));
		statbuf->st_mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;
		statbuf->st_nlink = 1;
		statbuf->st_uid = getuid();
		statbuf->st_gid = getgid();
		statbuf->st_size = 0;
		statbuf->st_blocks = 0;
		statbuf->st_atime = time(NULL);
		statbuf->st_mtime = time(NULL);
		statbuf->st_ctime = time(NULL);

		return -ENOENT;
	}
		
	memcpy( statbuf, &(inode_table[mapping].stat), sizeof(struct stat) );

    	return retstat;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int retstat = 0;
   	log_msg("\nsfs_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n", path, mode, fi);

	if(path_lookup(path) > 0) {
		return -EEXIST;
	}

	/* set file handle */
	fi->fh = get_fh();
	if(fi->fh < 0) {
		return -EMFILE;
	}

	int inode = get_inode();

	open_files[fi->fh].refcnt = 1;
	open_files[fi->fh].st_ino = inode;
	open_files[fi->fh].pos = 0;

	/* enter file  into inode table */
        inode_table[inode].stat.st_mode = mode;
        inode_table[inode].stat.st_nlink = 1;
        inode_table[inode].stat.st_uid = getuid();
        inode_table[inode].stat.st_gid = getgid();
        inode_table[inode].stat.st_size = 0;
        inode_table[inode].stat.st_blocks = 0;
	inode_table[inode].stat.st_atime = time(NULL);
	inode_table[inode].stat.st_mtime = time(NULL);
	inode_table[inode].stat.st_ctime = time(NULL);

        /* enter file into name table */
	int mapping = get_mapping();        
	if(mapping < 0) {
		free_inode(inode);
		free_fh(fi->fh);
		return -ENOMEM;
	}
	sprintf(name_table[mapping].path, path);
        name_table[mapping].st_ino = inode;

	/* get info on current working directory */
        char cwd_path[255];
        memset(cwd_path, 0, 255);
        get_cwd_path(path, cwd_path);
        mapping = path_lookup(cwd_path);
        cwd_inode = name_table[mapping].st_ino;

	/* enter file into parent directory  */
	int i = 0;
	void *buf = malloc(BLOCK_SIZE*2);
	memset(buf, 0, BLOCK_SIZE);
	block_read(get_block_index(&(inode_table[cwd_inode]), i), buf);
        char *ptr = (char*)buf;
        while(*ptr != '\0') {
                ptr++;
                // check to make sure we have not gone off the end of the block
                // and read in a new block if needed
                if ((void *)ptr >= buf + BLOCK_SIZE) {
                        block_read(get_block_index(&(inode_table[cwd_inode]),++i), buf);
                        ptr = (char*)buf;
                }
        }
        /************************************************
        * Need to check if the new entry will exceed    *
        * the buffer and require a new block to be      *
        * allocated.                                    *
        * **********************************************/

        char name[255];
	short block;

        memset(name, 0, 255);
        get_name(path, name);
	sprintf(ptr, "/%s/%i", name, inode);
	int size = strlen(ptr) + 1;

       	block_write(get_block_index(&(inode_table[cwd_inode]), i), buf);

	// in case the new entry requires a new block to be allocated to the directory
	if (inode_table[cwd_inode].stat.st_size%BLOCK_SIZE + size > BLOCK_SIZE) {
		block = get_block();
		set_block_index(&(inode_table[cwd_inode]), ++i, block);
		block_write(block, buf+BLOCK_SIZE);
	}

	inode_table[cwd_inode].stat.st_size += size;
        inode_table[cwd_inode].stat.st_nlink++;

    	return retstat;
}

/** Remove a file */
int sfs_unlink(const char *path)
{
	int retstat = 0;
    	log_msg("sfs_unlink(path=\"%s\")\n", path);

	/* path lookup  */
	int mapping = path_lookup(path);
	if(mapping < 0) {
		return -ENOENT;
	}	

	/* update parent directory */
	char *entry = malloc(MAX_PATH_LEN);
	memset(entry, 0, MAX_PATH_LEN);
	char name[255];
	memset(name, 0, 255);
	char cwd_path[255];
	memset(cwd_path, 0, 255);

	get_name(path, name);
	sprintf(entry, "/%s/%i", name, name_table[mapping].st_ino);	// assemble directory entry to be removed

	get_cwd_path(path, cwd_path);
	int cwd_mapping = path_lookup(cwd_path);
	cwd_inode = name_table[cwd_mapping].st_ino;

	int size = inode_table[cwd_inode].stat.st_size;
	int blocks = size/BLOCK_SIZE + (size%BLOCK_SIZE) ? 1 : 0;
	void * buf = malloc(BLOCK_SIZE*blocks);
	memset(buf, 0, BLOCK_SIZE*blocks);
	int i;
	for (i = 0; i < blocks; i++) {
		block_read(get_block_index(&(inode_table[cwd_inode]), i), buf + i*BLOCK_SIZE);	
	}
	removeSubstring((char*)buf, entry);
	int resize = BLOCK_SIZE-strlen((char*)buf);
	memset(buf+strlen((char*)buf), 0, resize);

	for (i=0; i < blocks; i++) {
		block_write(get_block_index(&(inode_table[cwd_inode]), i), buf + i*BLOCK_SIZE);
	}

	// if the resize frees up a block, free said block in the metadata
	if (size/BLOCK_SIZE > (size - resize)/BLOCK_SIZE) {
		disk_blocks[get_block_index(&(inode_table[cwd_inode]), i)] = FREE;
		set_block_index(&(inode_table[cwd_inode]), i, -1);
	}
	inode_table[cwd_inode].stat.st_size -= resize;
	inode_table[cwd_inode].stat.st_nlink--;

	/* update tables  */
	free_inode(name_table[mapping].st_ino);		// Eric please change this to your double indirection thing
	free_mapping(mapping);
 
    	return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int sfs_open(const char *path, struct fuse_file_info *fi)
{
    	int retstat = 0;
    	log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n", path, fi);

	int mapping = path_lookup(path);
	if(mapping < 0) {
		return -ENOENT;
	}

	/* test permissions against mode  */
	int gid = getgid();
	int uid = getuid();
	struct stat *stats = &(inode_table[name_table[mapping].st_ino].stat);

	if(access(path, R_OK) == EACCES) {
		return -EACCES;
	}
	
	if( S_ISDIR(stats->st_mode) ) {
		return -EISDIR;
	}

	/* allocate file handle */	
	retstat = get_fh();
	if(retstat < 0) {
		return -ENOMEM;
	} else {
		fi->fh = retstat;
	}

	open_files[fi->fh].refcnt = 1;
	open_files[fi->fh].st_ino = name_table[mapping].st_ino; 
	open_files[fi->fh].pos = 0;	
    
    	return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int sfs_release(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;
    	log_msg("\nsfs_release(path=\"%s\", fi=0x%08x)\n", path, fi);
    	
	free_fh(fi->fh);	

	return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;
    	log_msg("\nsfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);

	// check range
        if(fi->fh < 0 || fi->fh > MAX_LINK-1) {
                return -EBADF;
        }

        // check if open
        if(open_files[fi->fh].st_ino < 0) {
                return -EBADF;
        }

        struct stat *stat = &(inode_table[open_files[fi->fh].st_ino].stat);

        if(offset >= stat->st_size) {
                return EOF;
        }	

	// read would go beyong range of file
	if(offset + size > stat->st_size) {
		size = stat->st_size - offset;
	}

	/* calculate starting block */
	int start_block = offset/BLOCK_SIZE;    // truncated 
        int block_offset = offset % BLOCK_SIZE; // offset within starting block

	/* read blocks into aligned buffer  */
	int read_blocks =
                ((block_offset) ? 1 : 0) +                                              // first block if partial
                (size-((block_offset) ? (BLOCK_SIZE-block_offset) : 0))/BLOCK_SIZE +    // full blocks
                (((offset+size)%BLOCK_SIZE) ? 1 : 0);					// last block if partial
	void *aligned_buf = malloc(read_blocks*BLOCK_SIZE);	// block-aligned buffer
	char *pos = (char*)aligned_buf;
	int i = 0;
	while(read_blocks > 0) {
		block_read(get_block_index(&(inode_table[open_files[fi->fh].st_ino]), start_block+i), (void *)pos);
		// block_read(inode_table[open_files[fi->fh].st_ino].blocks[start_block+i], (void*)pos);
                pos += BLOCK_SIZE;
		read_blocks--;
		i++;
	}

	memcpy(buf, aligned_buf + block_offset, size);
    	return size;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int sfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;
    	log_msg("\nsfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);

	// check range
	if(fi->fh < 0 || fi->fh > MAX_LINK-1) {
		return -EBADF;
	}

	// check if open
	if(open_files[fi->fh].st_ino < 0) {
		return -EBADF;
	}

	struct stat *stat = &(inode_table[open_files[fi->fh].st_ino].stat);

	if(offset > stat->st_size) {
		return 0;
	}

	// calculate starting block
	int start_block = offset/BLOCK_SIZE; 	// truncated 
	int block_offset = offset % BLOCK_SIZE;	// offset within starting block

	// file needs to be resized
	void* block_buf = malloc(BLOCK_SIZE);
	int i;
	if(offset + size > ((stat->st_size)/BLOCK_SIZE + (((stat->st_size)%BLOCK_SIZE) ? 1 : 0))*BLOCK_SIZE) {
		int blocks_needed = (offset + size)/BLOCK_SIZE - stat->st_size/BLOCK_SIZE - ((stat->st_size%BLOCK_SIZE) ? 1 : 0) + (((offset + size)%BLOCK_SIZE) ? 1 : 0);
		int blocks_left = blocks_needed;	
	
		i = stat->st_blocks;
		int block;
		while(blocks_left > 0) {
			block = get_block();
			if(block < 0) {
				return -ENOMEM;
			}
	
			if(i > MAX_FILE_BLOCKS) {
				return -EFBIG;
			}
			
			set_block_index(&(inode_table[open_files[fi->fh].st_ino]), i, block);
			i++;
			blocks_left--;
			stat->st_blocks++;
		}

	}

	/* load file blocks being written to into continuous buffer */
	int write_blocks = 								
		((block_offset) ? 1 : 0) + 						// first block if partial
		(size-((block_offset) ? (BLOCK_SIZE-block_offset) : 0))/BLOCK_SIZE + 	// full blocks
		(((offset+size)%BLOCK_SIZE) ? 1 : 0);					// last block if partial
	void *cont_buffer = malloc(write_blocks*BLOCK_SIZE);
	memset(cont_buffer, 0, write_blocks*BLOCK_SIZE);
	char *pos = (char*)cont_buffer;
		
	block_read(get_block_index(&(inode_table[open_files[fi->fh].st_ino]), start_block), cont_buffer);
	
	/* write to continuous buffer */
	memcpy(cont_buffer + block_offset, buf, size);

	/* write back to disk */
	pos = (char*)cont_buffer;	
	for(i = 0; i < write_blocks; i++) {
		block_write(get_block_index(&(inode_table[open_files[fi->fh].st_ino]), start_block+i), (void *)pos);
		pos += BLOCK_SIZE;
        }

	stat->st_size += size;

	free(block_buf);
	free(cont_buffer);

	return size;
}


/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
	int retstat = 0;
    	log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n", path, mode);
  
	if(path_lookup(path) > 0) {
                return -EEXIST;
        }


        int inode = get_inode();

        /* enter file  into inode table */
        inode_table[inode].stat.st_mode = mode | S_IFDIR;
        inode_table[inode].stat.st_nlink = 2;
        inode_table[inode].stat.st_uid = getuid();
        inode_table[inode].stat.st_gid = getgid();
        inode_table[inode].stat.st_size = 512;
        inode_table[inode].stat.st_blocks = 1;
        inode_table[inode].stat.st_atime = time(NULL);
        inode_table[inode].stat.st_mtime = time(NULL);
        inode_table[inode].stat.st_ctime = time(NULL);
	
	inode_table[inode].blocks[0] = get_block();

	/* enter file into name table */
        int mapping = get_mapping();
        if(mapping < 0) {
                free_inode(inode);
                return -ENOMEM;
        }
        sprintf(name_table[mapping].path, path);
        name_table[mapping].st_ino = inode;

	/* get info on current working directory */
	char cwd_path[255];
	memset(cwd_path, 0, 255);
	get_cwd_path(path, cwd_path);
        mapping = path_lookup(cwd_path);
        cwd_inode = name_table[mapping].st_ino;

	/* enter . and .. */
	void *buf = malloc(BLOCK_SIZE);
        memset(buf, 0, BLOCK_SIZE);
	sprintf(buf, "./%i/../%i", inode, cwd_inode);
        block_write(get_block_index(&(inode_table[inode]), 0), buf);

        /* enter file into parent directory  */
        int i = 0;
        memset(buf, 0, BLOCK_SIZE);
        
	block_read(get_block_index(&(inode_table[cwd_inode]), i), buf);
        char *ptr = (char*)buf;
        while(*ptr != '\0') {
                ptr++;
                // check to make sure we have not gone off the end of the block
                // and read in a new block if needed
                if ((void *)ptr >= buf + BLOCK_SIZE) {
                        block_read(get_block_index(&(inode_table[cwd_inode]),++i), buf);
                        ptr = (char*)buf;
                }
        }
        /************************************************
        * Need to check if the new entry will exceed    *
        * the buffer and require a new block to be      *
        * allocated.                                    *
        * **********************************************/

        char name[255];
        short block;

        memset(name, 0, 255);
        get_name(path, name);
        sprintf(ptr, "/%s/%i", name, inode);
        int size = strlen(ptr) + 1;

        block_write(get_block_index(&(inode_table[cwd_inode]), i), buf);

        // in case the new entry requires a new block to be allocated to the directory
        if (inode_table[cwd_inode].stat.st_size%BLOCK_SIZE + size > BLOCK_SIZE) {
        	block = get_block();
                set_block_index(&(inode_table[cwd_inode]), ++i, block);
                block_write(block, buf+BLOCK_SIZE);
        }
        
        inode_table[cwd_inode].stat.st_size += size;
        inode_table[cwd_inode].stat.st_nlink++;

    	return retstat;
}


/** Remove a directory */
int sfs_rmdir(const char *path)
{
    	int retstat = 0;
        log_msg("sfs_unlink(path=\"%s\")\n", path);

        /* path lookup  */
        int mapping = path_lookup(path);
        if(mapping < 0) {
                return -ENOENT;
        }

        /* update parent directory */
        char *entry = malloc(MAX_PATH_LEN);
        memset(entry, 0, MAX_PATH_LEN);
        char name[255];
        memset(name, 0, 255);
        char cwd_path[255];
        memset(cwd_path, 0, 255);

        get_name(path, name);
        sprintf(entry, "/%s/%i", name, name_table[mapping].st_ino);     // assemble directory entry to be removed

        get_cwd_path(path, cwd_path);
        int cwd_mapping = path_lookup(cwd_path);
        cwd_inode = name_table[cwd_mapping].st_ino;

        int size = inode_table[cwd_inode].stat.st_size;
        int blocks = size/BLOCK_SIZE + (size%BLOCK_SIZE) ? 1 : 0;
        void * buf = malloc(BLOCK_SIZE*blocks);
	memset(buf, 0, BLOCK_SIZE*blocks);
        int i;
        for (i = 0; i < blocks; i++) {
                block_read(get_block_index(&(inode_table[cwd_inode]), i), buf + i*BLOCK_SIZE);
        }

	/* test to see if directory is empty */
	int count = 0;
	char *pos = (char*)buf;
	for(i = 0; i < strlen((char*)buf); i++) {
	        if(pos[i] == '/') {
			count++;
		}
	}

	if(inode_table[name_table[mapping].st_ino].stat.st_nlink > 2) {
		return -ENOTEMPTY;
	}

        removeSubstring((char*)buf, entry);
        int resize = BLOCK_SIZE-strlen((char*)buf);
        memset(buf+strlen((char*)buf), 0, resize);

        for (i=0; i < blocks; i++) {
                block_write(get_block_index(&(inode_table[cwd_inode]), i), buf + i*BLOCK_SIZE);
        }

        // if the resize frees up a block, free said block in the metadata
        if (size/BLOCK_SIZE > (size - resize)/BLOCK_SIZE) {
        	disk_blocks[get_block_index(&(inode_table[cwd_inode]), i)] = FREE;
                set_block_index(&(inode_table[cwd_inode]), i, -1);
        }
        inode_table[cwd_inode].stat.st_size -= resize;
        inode_table[cwd_inode].stat.st_nlink--;

        /* update tables  */
        free_inode(name_table[mapping].st_ino);         // Eric please change this to your double indirection thing
        free_mapping(mapping);

	free(entry);
	free(buf);
        return retstat;	
}

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int sfs_opendir(const char *path, struct fuse_file_info *fi)
{
    	int retstat = 0;
    	log_msg("\nsfs_opendir(path=\"%s\", fi=0x%08x)\n", path, fi);
   
    	return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    	int retstat = 0, mapping, st_ino;
	mapping = path_lookup(path);
	if(mapping < 0) {
		return -ENOENT;
	}
	
	st_ino = name_table[mapping].st_ino;

	int i, blocks = inode_table[st_ino].stat.st_size/BLOCK_SIZE + (inode_table[st_ino].stat.st_size%BLOCK_SIZE) ? 1 : 0;

	char *entry_list = malloc(BLOCK_SIZE*blocks), *tok;
	for (i = 0; i < blocks; i++) {
		block_read(inode_table[st_ino].blocks[0] , entry_list + i*BLOCK_SIZE);
	}
	
	tok = strtok(entry_list, "/");

	while(tok != NULL) {
		filler( buf, tok, NULL, 0 );
		strtok(NULL, "/");
		tok = strtok(NULL, "/");
	}

	free(entry_list);
	
    	return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int sfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;

    
    return retstat;
}

struct fuse_operations sfs_oper = {
  .init = sfs_init,
  .destroy = sfs_destroy,

  .getattr = sfs_getattr,
  .create = sfs_create,
  .unlink = sfs_unlink,
  .open = sfs_open,
  .release = sfs_release,
  .read = sfs_read,
  .write = sfs_write,

  .rmdir = sfs_rmdir,
  .mkdir = sfs_mkdir,

  .opendir = sfs_opendir,
  .readdir = sfs_readdir,
  .releasedir = sfs_releasedir
};

void sfs_usage()
{
    fprintf(stderr, "usage:  sfs [FUSE and mount options] diskFile mountPoint\n");
    abort();
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct sfs_state *sfs_data;
    
    // sanity checking on the command line
    if ((argc < 3) || (argv[argc-2][0] == '-') || (argv[argc-1][0] == '-'))
	sfs_usage();

    sfs_data = malloc(sizeof(struct sfs_state));
    if (sfs_data == NULL) {
	perror("main calloc");
	abort();
    }

    // Pull the diskfile and save it in internal data
    sfs_data->diskfile = argv[argc-2];
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;
    
    sfs_data->logfile = log_open();
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, %s \n", sfs_data->diskfile);
    fuse_stat = fuse_main(argc, argv, &sfs_oper, sfs_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    
    return fuse_stat;
}

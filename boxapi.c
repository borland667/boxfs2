/***************************************

  DR: Interazione con il sito
  
  This software is licensed under the 
  GPLv2 license.

***************************************/

#include "boxapi.h"
#include "boxpath.h"
#include "boxhttp.h"
#include "boxopts.h"
#include "boxjson.h"
#include "boxutils.h"
#include "boxcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <termios.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>

#include <libapp/app.h>
/* Building blocks for OpenBox api endpoints
   and return codes
*/
// -- v2 --
//    AUTH
#define API_KEY_VAL "f9ss11y2w0hg5r04jsidxlhk4pil28cf"
#define API_SECRET  "r3ZHAIhsOL2FoHjgERI9xf74W5skIM0w"
#define API_OAUTH_URL "https://www.box.com/api/oauth2/"
#define API_OAUTH_AUTHORIZE API_OAUTH_URL "authorize?response_type=code&client_id=" API_KEY_VAL "&redirect_uri=http%3A//localhost"
#define API_OAUTH_TOKEN     API_OAUTH_URL "token"
//    CALLS
#define API_ENDPOINT	"https://api.box.com/2.0/"
#define API_LS		API_ENDPOINT "folders/"
#define API_INFO	API_ENDPOINT "users/me"
#define API_DOWNLOAD	API_ENDPOINT "files/%s/content"
#define API_UPLOAD      "https://upload.box.com/api/2.0/files/content"
//    UTILS
#define BUFSIZE 1024
// -- v1 --
//#define API_KEY_VAL "2uc9ec1gtlyaszba4h6nixt7gyzq3xir"
#define API_KEY "&api_key=" API_KEY_VAL
#define API_TOKEN API_KEY "&auth_token="
#define API_REST_BASE "://www.box.net/api/1.0/rest?action="
#define API_GET_ACCOUNT_TREE API_REST_BASE "get_account_tree&folder_id=0" API_TOKEN
#define API_GET_ACCOUNT_TREE_OK "listing_ok"
//#define API_DOWNLOAD "://www.box.net/api/1.0/download/"
//#define API_UPLOAD "://upload.box.net/api/1.0/upload/"
#define API_CREATE_DIR API_REST_BASE "create_folder" API_TOKEN
#define API_CREATE_DIR_OK "create_ok"
#define API_RENAME API_REST_BASE "rename" API_TOKEN
#define API_RENAME_OK "s_rename_node"
#define API_MOVE API_REST_BASE "move" API_TOKEN
#define API_MOVE_OK "s_move_node"
#define API_UNLINK API_REST_BASE "delete&target=file" API_TOKEN
#define API_UNLINK_OK "s_delete_node"
#define API_RMDIR API_REST_BASE "delete&target=folder" API_TOKEN
#define API_RMDIR_OK API_UNLINK_OK 
#define API_LOGOUT API_REST_BASE "logout" API_KEY "&auth_token="

#define LOCKDIR(dir) pthread_mutex_lock(dir->dirmux);
#define UNLOCKDIR(dir) pthread_mutex_unlock(dir->dirmux); 

/* globals, written during initialization */
char *auth_token = NULL, *refresh_token = NULL;
char treefile[] = "/tmp/boxXXXXXX";
long long tot_space, used_space;
struct box_options_t options;

char * tag_value(const char * buf, const char * tag)
{
  char * tmp;
  char * sp, * ep;
  
  sp = strstr(buf,tag)+strlen(tag)+1;
  ep = strstr(sp,"<");
  tmp =malloc(ep-sp+1);
  strncpy(tmp, sp, ep-sp);
  tmp[ep-sp]=0;
  
  return tmp;
}


long long tag_lvalue(const char * buf, const char * tag)
{
  char * tmp;
  long long rv;
  
  tmp = tag_value(buf, tag);
  rv = atoll(tmp);
  free(tmp);
  
  return rv;  
}

char * attr_value(const char * buf, const char * attr)
{
  char * tmp;
  char * sp, * ep;
  char ss[24];

  sprintf(ss,"%s=\"", attr);
  sp = strstr(buf,ss);// +strlen(ss);
  if(!sp) return NULL;
  else sp = sp + strlen(ss);
  ep = strstr(sp,"\"");
  tmp =malloc(ep-sp+1);
  strncpy(tmp, sp, ep-sp);
  tmp[ep-sp]=0;

  return tmp;
}

int ends_with(const char * str, const char * suff)
{
        char * sub = strstr(str, suff);
        if(!sub) return FALSE;
        return !strcmp(suff, sub);
}


/* OBSOLETE
void api_logout()
{
  char * buf;

  buf = http_fetchf("%s" API_LOGOUT "%s", proto, auth_token);
  free(buf);
}
*/

void api_free()
{
	//api_logout();

	if(auth_token) free(auth_token);
	if(refresh_token) free(refresh_token);
	syslog(LOG_INFO, "Unmounting filesystem");
	closelog();
  
	xmlCleanupParser();
	if(allDirs) xmlHashFree(allDirs, NULL); // TODO: Deallocator!
}

/* only for 1st level nodes! */
char * node_value(const char * buf, const char * name)
{

  char * val = NULL;
  
  xmlDoc *doc = NULL;
  xmlNode *root_element = NULL;
  xmlNode *cur_node = NULL;
  
  doc = xmlReadDoc(buf, "noname.xml",NULL, 0);

  root_element = xmlDocGetRootElement(doc);
  if(!root_element) {
    if(doc) xmlFreeDoc(doc);
    return val;
  }

  for(cur_node = root_element->children; cur_node && !val; cur_node = cur_node->next) {
      if (cur_node->type == XML_ELEMENT_NODE) {
        if(!strcmp(name,cur_node->name)) { 
          // a nice thing of text nodes :(((
          val = (cur_node->content ? strdup(cur_node->content) : 
                            strdup(cur_node->children->content)); 
        }
      }
  }
  
  xmlFreeDoc(doc);
   
  return val;   
}

/* APIv2 
 * Handle Oauth2 authentication
 */
void save_tokens(const char * token_file)
{
	FILE * tf = fopen(token_file, "w");
	
	if(tf) {
		fprintf(tf, "%s\n%s\n", auth_token, refresh_token);
		fclose(tf);
	}
}
 
int get_oauth_tokens()
{
	int res = 0;
	char * buf = NULL, * code = NULL;
	jobj * tokens;
	postdata_t postpar=post_init();

	printf("Visit %s to authorize, then paste the code below\n", API_OAUTH_AUTHORIZE);
	code = app_term_askpass("Code:");

	post_add(postpar, "grant_type", "authorization_code");
	post_add(postpar, "code", code);
	post_add(postpar, "client_id", API_KEY_VAL);
	post_add(postpar, "client_secret", API_SECRET);
	buf = http_post(API_OAUTH_TOKEN, postpar);
	//printf("Response: %s\n", buf);
	tokens = jobj_parse(buf);
	if(tokens) {
		auth_token = jobj_getval(tokens, "access_token");
		refresh_token = jobj_getval(tokens, "refresh_token");
		if(auth_token) {
			if(options.verbose) syslog(LOG_DEBUG, "auth_token=%s - refresh_token=%s\n",
				auth_token, refresh_token);
			if(options.token_file) save_tokens(options.token_file);
		} else {
			char * err = jobj_getval(tokens, "error_description");
			fprintf(stderr, "Unable to get access token: %s\n", err ? err : "unknown error");
		}
		jobj_free(tokens);
	} else {
        	fprintf(stderr, "Unable to parse server response:\n%s\n", buf);
	}

	post_free(postpar);
	if(buf)    free(buf);
	if(code)   free(code);
	return res;
}

int refresh_oauth_tokens()
{
	int res = 0;
	char * buf = NULL;
	jobj * tokens;
	postdata_t postpar=post_init();

	post_add(postpar, "grant_type", "refresh_token");
	post_add(postpar, "refresh_token", refresh_token);
	post_add(postpar, "client_id", API_KEY_VAL);
	post_add(postpar, "client_secret", API_SECRET);
	buf = http_post(API_OAUTH_TOKEN, postpar);
	//printf("Response: %s\n", buf);
	tokens = jobj_parse(buf);
	if(tokens) {
		auth_token = jobj_getval(tokens, "access_token");
		refresh_token = jobj_getval(tokens, "refresh_token");
		if(auth_token) {
			if(options.verbose) syslog(LOG_DEBUG, "auth_token=%s - refresh_token=%s\n",
				auth_token, refresh_token);
			if(options.token_file) save_tokens(options.token_file);
		} else {
			char * err = jobj_getval(tokens, "error_description");
			syslog(LOG_ERR, "Unable to get access token: %s\n", err ? err : "unknown error");
		}
		jobj_free(tokens);
	} else {
        	fprintf(stderr, "Unable to parse server response:\n%s\n", buf);
	}

	post_free(postpar);
	if(buf)    free(buf);
	return res;
}


jobj * get_account_info() {
	char * buf = NULL;
	jobj * o;

	buf = http_fetch(API_INFO);
	o = jobj_parse(buf);
	free(buf);
	
	return o;
}

void api_getusage(long long * tot_sp, long long * used_sp)
{
  *tot_sp = tot_space;
  *used_sp = used_space;
}

int api_createdir(const char * path)
{
  int res = 0;
  boxpath * bpath;
  boxdir *newdir;
  boxfile * aFile;
  char * dirid, *buf, *status;

  bpath = boxpath_from_string(path);
  if(bpath->dir) {
	//syslog(LOG_WARNING, "creating dir %s (escaped: %s) ",base,xmlURIEscapeStr(base,""));
    buf = http_fetchf(API_CREATE_DIR "%s&parent_id=%s&name=%s&share=0", 
          auth_token, bpath->dir->id, xmlURIEscapeStr(bpath->base,""));
    status = node_value(buf,"status");
    if(strcmp(status,API_CREATE_DIR_OK)) {
      res = -EINVAL;
      free(buf); free(status);
      boxpath_free(bpath);
      return res;
    }
    free(status);

    dirid = tag_value(buf,"folder_id");
    free(buf);
    
    // aggiungo 1 entry all'hash
    newdir = boxdir_create();
    newdir->id = dirid;
    xmlHashAddEntry(allDirs, path, newdir);
    // upd parent
    aFile = boxfile_create(bpath->base);
    aFile->id = strdup(dirid);
    LOCKDIR(bpath->dir);
    list_append(bpath->dir->folders, aFile);
    UNLOCKDIR(bpath->dir);    
  } else {
    syslog(LOG_WARNING, "UH oh... wrong path %s",path);
    res = -EINVAL;
  }
  boxpath_free(bpath);

  return res;
}


int api_create(const char * path)
{
  int res = 0;
  boxpath * bpath = boxpath_from_string(path);
  boxfile * aFile;

  if(bpath->dir) {
    aFile = boxfile_create(bpath->base);
    LOCKDIR(bpath->dir);
    list_append(bpath->dir->files,aFile);
    UNLOCKDIR(bpath->dir);
  } else {
    syslog(LOG_WARNING, "UH oh... wrong path %s",path);
    res = -ENOTDIR;
  }
  boxpath_free(bpath);
  
  return res;
}

char * get_folder_info(const char * id, int items )
{
	char * buf = NULL;
	
	if(items) {
		buf = cache_get(id);
		if(buf) return buf;
		buf = http_fetchf(API_LS "%s/items?fields=size,name,created_at,modified_at", id);
		cache_put(id, buf);
		return buf;
	}
	
	return http_fetchf(API_LS "%s", id);
	return buf;
}

void set_filedata(const boxpath * bpath, char * res, long long fsize)
{
        boxfile * aFile;
        list_iter it = list_get_iter(bpath->dir->files);
        jobj * o = jobj_parse(res);
        
        if(!o) {
                syslog(LOG_ERR, "Unable to parse file data for %s", bpath->base);
        }
        o = jobj_get(o, "entries");
        if (o) o = jobj_array_item(o, 0); //first item
        for(; it; it = list_iter_next(it)) {
                aFile = (boxfile*)list_iter_getval(it);
                if(!strcmp(aFile->name, bpath->base)) {
                        aFile->id = jobj_getval(o, "id");
                        aFile->size = fsize;
                        return;
		}
	}
}

void set_partdata(const boxpath * bpath, char * res, const char * partname)
{
	boxfile * aFile = boxfile_create(partname);
	jobj * o = jobj_parse(res);

	if(!o) {
		syslog(LOG_ERR, "Unable to parse file data for %s", bpath->base);
	}
        o = jobj_get(o, "entries");
        if (o) o = jobj_array_item(o, 0); //first item
        
        aFile->id=jobj_getval(o, "id");
        LOCKDIR(bpath->dir);
        list_append(bpath->dir->pieces, aFile);
        UNLOCKDIR(bpath->dir);
}


int api_open(const char * path, const char * pfile){
	int res = 0;
	char gkurl[BUFSIZE]="";
	boxfile * aFile;
	list_iter it;
	boxpath * bpath = boxpath_from_string(path);

	if(!boxpath_getfile(bpath)) res = -ENOENT;
  
	if(!res) {
		sprintf(gkurl, API_DOWNLOAD, bpath->file->id);
		res = http_fetch_file(gkurl, pfile, FALSE);
		//NOTE: we could check for bpath->file->size > PART_LEN, but
		//checking filesize is more robust, since PART_LEN may change in
		//future, or become configurable.
		if(!res && options.splitfiles && bpath->file->size > filesize(pfile)) {
			//download of other parts
			for(it = boxpath_first_part(bpath); it ; it=boxpath_next_part(bpath, it)) {
				aFile = (boxfile*) list_iter_getval(it);
				sprintf(gkurl, API_DOWNLOAD, aFile->id);
				if(options.verbose) syslog(LOG_DEBUG, "Appending file part %s", aFile->name);
				http_fetch_file(gkurl, pfile, TRUE);
			}
		}
	}
  
	boxpath_free(bpath);  
	return res;
}

int api_readdir(const char * path, fuse_fill_dir_t filler, void * buf)
{
  int res = 0;
  boxdir * dir;
  boxfile * aFile;
  list_iter it;
  
  dir = (boxdir *) xmlHashLookup(allDirs,path);
  if (dir==NULL) return -EINVAL;

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  LOCKDIR(dir);
  for(it=list_get_iter(dir->folders); it; it = list_iter_next(it)) {
      aFile = (boxfile*)list_iter_getval(it);
      filler(buf, aFile->name, NULL, 0);
  }
  for(it=list_get_iter(dir->files); it; it = list_iter_next(it)) {
      aFile = (boxfile*)list_iter_getval(it);
      filler(buf, aFile->name, NULL, 0);
  }
  UNLOCKDIR(dir);

  return res;
}

int api_subdirs(const char * path)
{
  boxdir * dir;
  
  dir = (boxdir *) xmlHashLookup(allDirs,path);
  if (dir==NULL) return -1;

  return list_size(dir->folders);
}  

int api_getattr(const char *path, struct stat *stbuf)
{
	memset(stbuf, 0, sizeof(struct stat));	
	boxpath * bpath = boxpath_from_string(path);
	if(!bpath) return -ENOENT;
	if(!boxpath_getfile(bpath)) {
		boxpath_free(bpath);
		return -ENOENT;
	}
	stbuf->st_size = bpath->file->size;
	stbuf->st_ctime = bpath->file->ctime;
	stbuf->st_mtime = bpath->file->mtime;
	// access time unknown, approx with mtime
	stbuf->st_atime = bpath->file->mtime;
	stbuf->st_uid = options.uid;
	stbuf->st_gid = options.gid;
	if(bpath->is_dir) {
		stbuf->st_mode = S_IFDIR | options.dperm;
		stbuf->st_nlink = 2 + api_subdirs(path);
	} else {
		stbuf->st_mode = S_IFREG | options.fperm;
		stbuf->st_nlink = 1;
	}
	boxpath_free(bpath);
	return 0;
}

int api_removedir(const char * path)
{
  int res = 0;
  boxpath * bpath = boxpath_from_string(path);
  if(!boxpath_getfile(bpath)) return -EINVAL;
  
  char *buf, *status;
  
  if(!bpath->dir && !bpath->is_dir) return -ENOENT;
  buf = http_fetchf(API_RMDIR "%s&target_id=%s", auth_token, bpath->file->id);
  status = node_value(buf,"status");
  if(strcmp(status,API_UNLINK_OK)) {
    res = -EPERM;
  }
  free(status);
  free(buf);

  if(!res) {
    //remove it from parent's subdirs...
    LOCKDIR(bpath->dir);
    boxpath_removefile(bpath);
    UNLOCKDIR(bpath->dir);
    //...and from dir list
    xmlHashRemoveEntry(allDirs, path, NULL);
  }
  
  boxpath_free(bpath);  
  return res;
}

/*
 * Internal func to call the delete api on a file id.
 * api_removefile will call it several times if
 * splitfiles is on and the file has parts
 */
int do_removefile_id(const char * id)
{
	int res = 0;
	char *buf, *status;
	
	buf = http_fetchf(API_UNLINK "%s&target_id=%s", auth_token, id);
	status = node_value(buf,"status");
	if(strcmp(status,API_UNLINK_OK)) res = -ENOENT;
	
	free(status);
	free(buf);
	return res;
}

int api_removefile(const char * path)
{
	int res = 0;
	boxpath * bpath = boxpath_from_string(path);

	if(!bpath->dir) res = -ENOENT;
	else {
		//remove it from box.net
		boxpath_getfile(bpath);
		do_removefile_id(bpath->file->id);

		if(res==0) {
			if(options.splitfiles && list_size(bpath->dir->pieces)) {
				list_iter prev,cur;
				boxfile * part;

				//remove parts
				for(cur = boxpath_first_part(bpath); cur; ) {
					part = (boxfile*) list_iter_getval(cur);
					if (options.verbose) syslog(LOG_DEBUG, "removing part %s", part->name);
					do_removefile_id(part->id);
					prev = cur; cur = boxpath_next_part(bpath, cur);
					list_delete_item(bpath->dir->pieces, part);
				}
			}

			//remove it from the list
			LOCKDIR(bpath->dir);
			boxpath_removefile(bpath);
			UNLOCKDIR(bpath->dir);
		}
	}
	
	boxpath_free(bpath);
	//get_account_info();
	return res;
}

//Move and rename funcs, new version
int do_api_move_id(int is_dir, const char * srcid, const char * dstid)
{
	char * buf = NULL, * status;
	int res = 0;

	buf = http_fetchf(API_MOVE "%s&target=%s&target_id=%s&destination_id=%s", 
		  auth_token, (is_dir ? "folder" : "file"), srcid, dstid);
	status = node_value(buf,"status");
	if(strcmp(status,API_MOVE_OK)) {
	  res = -EINVAL;
	}

	free(status); free(buf);
	return res;
}

int do_api_move(boxpath * bsrc, boxpath * bdst)
{
	int res = 0;
	list_iter it;

	LOCKDIR(bsrc->dir);
	res = do_api_move_id(bsrc->is_dir, bsrc->file->id, bdst->dir->id);
	if(!res) {
		boxfile * part;
		//take care of parts, if any
		if(options.splitfiles && !bsrc->is_dir && list_size(bsrc->dir->pieces))
			for(it = boxpath_first_part(bsrc); it; it = boxpath_next_part(bsrc, it)) {
				part = (boxfile*)list_iter_getval(it);
				if(options.verbose) syslog(LOG_DEBUG, "Moving part %s", part->name);
				do_api_move_id(FALSE, part->id, bdst->dir->id);
				list_insert_sorted_comp(bdst->dir->pieces, part, filename_compare);
				list_delete_item(bsrc->dir->pieces, part);				
			}
		
		boxpath_removefile(bsrc);
		LOCKDIR(bdst->dir);
		list_append((bsrc->is_dir ? bdst->dir->folders : bdst->dir->files),
			bsrc->file);
		UNLOCKDIR(bdst->dir);
	}
	UNLOCKDIR(bsrc->dir);
	
	return res;
}

int do_api_rename_id(int is_dir, const char * id, const char * base)
{
	char * buf = NULL, * status;
	int res = 0;

	buf = http_fetchf(API_RENAME "%s&target=%s&target_id=%s&new_name=%s",
		  auth_token, (is_dir ? "folder" : "file"),
		  id, xmlURIEscapeStr(base,""));
	status = node_value(buf,"status");
	if(strcmp(status,API_RENAME_OK)) {
		res = -EINVAL;
	}

	free(status);free(buf);
	return res;	
}

int do_api_rename(boxpath * bsrc, boxpath * bdst)
{
	int res;
	
	LOCKDIR(bsrc->dir);
	res = do_api_rename_id(bsrc->is_dir, bsrc->file->id, bdst->base);
	if(!res) {
		boxfile * part;
		char * newname;
		list_iter it, prev;
		int ind=1;
		//take care of parts, if any
		if(options.splitfiles && !bsrc->is_dir && list_size(bsrc->dir->pieces))
			for(it = boxpath_first_part(bsrc); it; ) {
				part = (boxfile*)list_iter_getval(it);
				newname = malloc(strlen(bdst->base)+ PART_SUFFIX_LEN +4);
				sprintf(newname, "%s.%.2d" PART_SUFFIX, bdst->base, ind++);
				if(options.verbose) syslog(LOG_DEBUG, "Renaming part %s to %s", part->name, newname);
				do_api_rename_id(FALSE, part->id, newname);
				prev = it; it = boxpath_next_part(bsrc, it);
				list_delete_item(bsrc->dir->pieces, part);
				part->name = newname;
				list_insert_sorted_comp(bsrc->dir->pieces, part, filename_compare);
			}

		boxpath_renamefile(bsrc, bdst->base);
	}
	UNLOCKDIR(bsrc->dir);

	return res;
}

int api_rename_v2(const char * from, const char * to)
{
	int res = 0;
	boxpath * bsrc = boxpath_from_string(from);
	boxpath * bdst = boxpath_from_string(to);
	if(!boxpath_getfile(bsrc)) return -EINVAL; 
	boxpath_getfile(bdst);

	if(bsrc->dir!=bdst->dir) {
		res=do_api_move(bsrc, bdst);
	}
	if(!res && strcmp(bsrc->base, bdst->base)) {
		res = do_api_rename(bsrc,bdst);
	}
	if(!res && bsrc->is_dir) {
	    boxtree_movedir(from, to);
	}

	boxpath_free(bsrc);
	boxpath_free(bdst);
	return res;
}

void api_upload(const char * path, const char * tmpfile)
{
  postdata_t buf = post_init();
  char * res = NULL, * pr = NULL, * partname="";
  off_t fsize;
  size_t start, len;
  int oldver;
  jobj * part;
  boxpath * bpath = boxpath_from_string(path);

  if(bpath->dir) {
    post_add(buf, "parent_id", bpath->dir->id);
    fsize = filesize(tmpfile);
    oldver = boxpath_getfile(bpath);
    //if there was an older version of the file with parts, remove them
    if(options.splitfiles && oldver && (bpath->file->size > PART_LEN)) {
    	api_removefile(path);
    }
    //upload file in parts if needed
    if(options.splitfiles && fsize > PART_LEN) {
        post_addfile_part(buf, bpath->base, tmpfile, 0, PART_LEN);
        res = http_postfile(API_UPLOAD, buf);
        //fid = attr_value(res,"id");
        set_filedata(bpath ,res, fsize);
        free(res);
        start = PART_LEN;
        while(start < fsize-1) {
            post_free(buf); buf = post_init();
            post_add(buf, "parent_id", bpath->dir->id);
            
            partname = (char*) malloc(strlen(bpath->base)+PART_SUFFIX_LEN+4);
            sprintf(partname, "%s.%.2d%s", bpath->base, (int)(start/PART_LEN), PART_SUFFIX);

            if(options.verbose) syslog(LOG_DEBUG, "Uploading file part %s", partname);
            len = MIN(PART_LEN, fsize-start);
            pr = post_addfile_part(buf, partname, tmpfile, start, len);
            res = http_postfile(API_UPLOAD, buf);
            //fid = attr_value(res,"id");
            set_partdata(bpath, res, partname);

            free(pr); free(res); free(partname);
            start = start + len;
        }
    } else if(fsize) {
    	//normal upload
    	post_addfile(buf, bpath->base, tmpfile);
    	res = http_postfile(API_UPLOAD, buf);
	    //fid = attr_value(res,"id");
	    set_filedata(bpath ,res, fsize);
	    free(res);
    }
  } else {
    syslog(LOG_ERR,"Couldn't upload file %s",bpath->base);
  }
  post_free(buf);
  boxpath_free(bpath);
  //get_account_info();
}

void do_add_folder(const char * path, const char * id)
{
	char * buf;
	jobj * obj;
	boxdir * dir;
	boxfile * f;
	list_iter it;
	
	buf = get_folder_info(id, true);
	obj = jobj_parse(buf);
	free(buf);
	
	if(obj) {
	  	dir = boxtree_add_folder(path, id, obj);
	  	jobj_free(obj);
	  	it = list_get_iter(dir->folders);
	  	for(; it; it = list_iter_next(it)) {
	  	        f = list_iter_getval(it);
	  	        buf = pathappend(path, f->name);
	  		do_add_folder(buf, f->id);
	  		free(buf);
	  	}
	}
}


/*
 * Login to box.net, get the auth_token
 */
int api_init(int* argc, char*** argv) {

	int res = 0;

	/* parse command line arguments */
	if (parse_options (argc, argv, &options))
		return 1;  
  
	xmlInitParser();
	openlog("boxfs", LOG_PID, LOG_USER);
	cache_init(options.cache_dir);

	if(!auth_token || !refresh_token) 
  		res = get_oauth_tokens();
        else
        	res = refresh_oauth_tokens();

  	if(auth_token) {
  		char * buf;
  		jobj * root, *info;
  		
        	update_auth_header(auth_token);
        	buf = get_folder_info("0", false);
        	info = get_account_info();
        	root = jobj_parse(buf);
        	tot_space = jobj_getlong(info, "space_amount");
        	used_space = jobj_getlong(info, "space_used");
  		boxtree_init(root, info);
  		jobj_free(root); jobj_free(info);
  		free(buf);

  		do_add_folder("/", "0");

  		syslog(LOG_INFO, "Filesystem mounted on %s", options.mountpoint);
	}
  
	return res;
}


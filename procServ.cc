// Process server for soft ioc
// David H. Thompson 8/29/2003
// GNU Public License applies - see www.gnu.org


#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h> 
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>

#include <sys/wait.h> 
#include <signal.h>
#include <unistd.h> 
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>

#include "procServ.h"

bool inDebugMode;       // This enables a lot of printfs
bool logPortLocal;      // This restricts log port access to localhost
char *procservName;     // The name of this beast
char *childName;        // The name of that beast
char infoMessage1[512]; // This is sent to the user at sign on
char infoMessage2[512]; // This is sent to the user at sign on
int logfileFD=-1;
pid_t daemon_pid;

#define MAX_CONNECTIONS 64

// mLoop runs the program
void mLoop();
// Handles fatal errors in poll and also signals
void OnPollError();
// Handles houskeeping
void OnPollTimeout();
// Daemonizes the program
void forkAndGo();
int checkCommandFile(const char * command);

void OnSigChild(int);
int sigChildSet;

void OnSigPipe(int);
int sigPipeSet;
void OnSigUsr1(int);
int sigUsr1Set;

struct sigaction sig;


// this is the number of present connections
int ni;
char defaultpidFile[]="pid.txt";
void writePidFile()
{
    int pid=getpid();
    FILE * fp=NULL;
    char * pidFile=getenv("PROCSERV_PID");
    if (pidFile==NULL) pidFile=defaultpidFile;

    
    fp=fopen(pidFile,"w");
    // Dont stop here - just go without 
    if (fp==NULL) return;
    fprintf(fp,"%d\n",pid);
    fclose(fp);
}


int main(int argc,char * argv[])
{
    time(&procServStart); // What time is it now
    struct pollfd * pollList=NULL,* ppoll; // Allocate as much space as needed
    char cwd[512];
    int c;
    int ctlPort, logPort=0;
    char *command;
    bool wrongOption = false;

    procservName = argv[0];

    while (1) {
        static struct option long_options[] = {
            {"debug",         no_argument,       0, 'd'},
            {"logrestricted", no_argument,       0, 'r'},
            {"logport",       required_argument, 0, 'l'},
            {"name",          required_argument, 0, 'n'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;
     
        c = getopt_long (argc, argv, "drl:n:",
                         long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1) break;

        switch (c)
        {
        case 'd':
            inDebugMode = true;
            break;

        case 'r':
            logPortLocal = true;
            break;

        case 'l':
            logPort = atoi( optarg );
            if ( logPort < 1024 ) {
                fprintf( stderr,
                         "Invalid log port %d - disabling log port feature.\n", logPort );
                logPort = 0;
            }
            break;

        case 'n':
            childName = strdup( optarg );
            break;

        case '?':
            /* getopt_long already printed an error message. */
            break;

        default:
            abort ();
        }
    }

    ctlPort = atoi(argv[optind]);
    command = argv[optind+1];
    if ( childName == NULL ) childName = command;

    if ( (argc-optind) < 2 || ctlPort < 1024 ) 
    {
	printf("Usage: %s [options] <port> <command arguments ... >\n", argv[0]);
	exit(0);
    }

    if ( checkCommandFile( command ) ) exit( errno );

    sig.sa_handler=&OnSigChild;
    sigaction(SIGCHLD,&sig,NULL);
    sig.sa_handler=&OnSigPipe;
    sigaction(SIGPIPE,&sig,NULL);
    sig.sa_handler=&OnSigUsr1;
    sigaction(SIGUSR1,&sig,NULL);

    // Make an accept item to listen for control connections
    try
    {
	connectionItem *acceptItem = acceptFactory( ctlPort );
	AddConnection(acceptItem);
    }
    catch (int error)
    {
	perror("Caught an exception creating the initial control telnet port");
	fprintf(stderr,"Exiting with error code: %d\n",error);
	exit(error);
    }

    if ( logPort ) {
        // Make an accept item to listen for log connections
        try
        {
            connectionItem *acceptItem = acceptFactory( logPort, logPortLocal, true );
            AddConnection(acceptItem);
        }
        catch (int error)
        {
            perror("Caught an exception creating the initial log telnet port");
            fprintf(stderr,"Exiting with error code: %d\n",error);
            exit(error);
        }
    }

    daemon_pid=getpid();

    if ( getenv("PROCSERV_DEBUG") != NULL ) inDebugMode = true;
    if ( inDebugMode == false )
    {
	forkAndGo();
	writePidFile();
    }
    else
    {
	logfileFD = 1;          // Enables messages 
    }

    // Record some useful data for managers 
    sprintf(infoMessage1, "procServ: my pid is: %d" NL
	    "Startup directory: %s " NL 
	    "Startup command: %s " NL,
	    getpid(),
	    getcwd(cwd,sizeof(cwd)),
	    command);

    // Create and add the stdio item 
    // AddConnection(new  connectionItem(0)); // Connects stdin

    // Run here until somthing makes it die
    while(1)
    {
	char buf[100];
	int nPoll=0; // local copy of the # items to poll
	int nPollAlloc; // How big is pollList right now
	int pollStatus; // What poll returns

	connectionItem * p ;

	if (sigUsr1Set)
	{
	    printf("Failed to exec %s: Either the file does not exist or I have no execute permission\n", command );
	    printf("Exiting procServ!\n");
	    exit(0);
	}

	if (sigPipeSet>0)
	{
	    PRINTF("Got a sigPipe\n");
	    sprintf(buf,"Got a sigPipe signal: Did the IOC close its tty?" NL);
	    SendToAll(buf,strlen(buf),NULL);
	    sigPipeSet--;
	}
	// Adjust the poll data array
	if (nPollAlloc != ni)
	{
	    if (pollList) free(pollList);
	    pollList=(pollfd *) malloc(ni*sizeof(struct pollfd));
	    nPollAlloc=ni;
	}

	// Load the socket number and flags
	ppoll=pollList;
	nPoll=0;
	p = connectionItem::head;
	while(p)
	{
	    if (p->SetPoll(ppoll))
	    {
	    	ppoll++; // Move to the nexr fd slot
		nPoll++; // How many to poll
	    }
	    p=p->next;
	}
	
	// Now do the poll to find new data
	pollStatus=poll(pollList,nPoll,500);

	// handle what poll returns
	if (pollStatus<0) OnPollError();
	else if (pollStatus==0)
	{
	    // Go clean up dead connections
	    OnPollTimeout();
	    connectionItem * npi; 
	    
	    // Pick up the process item if it dies
	    // This call returns NULL if the process item lives
	    if (processFactoryNeedsRestart())
	    {
	    	npi= processFactory(argc-optind-2,argv+optind+1);
	    	if (npi) AddConnection(npi);
	    }
	}
	else
	{
	    // Loop thru all of the connectionItems until we find the one(s) that need to
	    // do I/O
	    p = connectionItem::head;
	    nPoll=0;
	    while(p && nPoll<pollStatus)
	    {
		if (p->OnPoll()) nPoll++;
		p=p->next;
	    }
	}
    }
}

// Connection items call this to send messages to others
// // This is a party line system, messages go to everyone
// // the sender's this pointer keeps it from getting its own
// // messages.
// //
void SendToAll(const char * message,int count,const connectionItem * sender)
{
    connectionItem * p = connectionItem::head;

    // Log the traffic 
    if (logfileFD>0 && (sender==NULL  || sender->isProcess() ))
        write( logfileFD, message, count );

    while(p)
    {
	if (p->isProcess()) 
	{
	    // Non-null senders that are not processes can send to processes
	    if (sender && !sender->isProcess()) p->Send(message,count);
	}
	else // Not a process
	{
	    // Null senders and processes can send to non-processes (ie connections)
	    if (sender==NULL || sender->isProcess() ) p->Send(message,count);
	}
	p=p->next;
    }
}


// Handles fatal errors in poll and also signals
void OnPollError()
{
	int pid;
	int status;

	switch (errno)
	{
	case EINTR:
		
		break;
	default:
		break;
	}
	
}
// Handles houskeeping
void OnPollTimeout()
{
    pid_t pid;
    int wstatus;
    connectionItem * p = connectionItem::head;
    char buf[128];
    
    if (sigChildSet)
    {
	pid = wait(&wstatus);
	while(p)
	{
	    p->OnWait(pid);
	    p=p->next;
	}
	sigChildSet--;

	sprintf(buf,NL "@@@@@@@@@@@@@" NL "Received a sigchild for process: %d." , pid);

	if (WIFEXITED(wstatus))
	{
	    sprintf(buf+strlen(buf)," Normal exit status=%d",WEXITSTATUS(wstatus));
	}
	if (WIFSIGNALED(wstatus))
	{
	    sprintf(buf+strlen(buf)," The process was killed by signal=%d",WTERMSIG(wstatus));
	}
	strcat(buf,NL "@@@@@@@@@@@@@" NL );
	SendToAll(buf,strlen(buf),NULL);
    }

    p = connectionItem::head;
    while (p)
    {
    
	if (p->IsDead())
	{
	    DeleteConnection(p);
	    // p is now invalid. 
	    // break the loop now and we will
	    // get another chance later.
	    break;
	}
	p=p->next;
    }

}

// Call this to add the item to the list of connections
void AddConnection(connectionItem * ci)
{
	if (connectionItem::head )
	{
	    ci->next=connectionItem::head;
	    ci->next->prev=ci;
	}
	else ci->next=NULL;
	
	ci->prev=NULL;
	connectionItem::head=ci;
	ni++;
	PRINTF("Adding connection\n");
}


void DeleteConnection(connectionItem *ci)
{
	if (ci->prev) // Not the head
	{
		ci->prev->next=ci->next;
	}
	else
	{
		connectionItem::head = ci->next;
	}
	if (ci->next) ci->next->prev=ci->prev;
	delete ci;
	ni--;
	assert(ni>=0);
	PRINTF("Deleting connection\n");
}

void OnSigChild(int)
{
	sigChildSet++;
}
void OnSigPipe(int)
{
	sigPipeSet++;
}
void OnSigUsr1(int)
{
    sigUsr1Set++;
}



// Fork the daemon and exit the parent
void forkAndGo()
{
    pid_t p=fork();
    char * buf="/dev/null";
    int fh;
    struct stat statBuf;

    fstat(1,&statBuf); // Find out what stdout is:

    if (p) // I am the parent
    {
	fprintf(stderr,"procServ spawining daemon process: %d\n",p);
	if (S_ISREG(statBuf.st_mode))
	    fprintf(stderr,"The open file on stdout will be used as a log file.\n");
	else
	    fprintf(stderr,"Stdout is not a file - no log will be kept.\n");

	exit(0);
    }
    daemon_pid=getpid();
    // p==0
    // The daemon starts up here

    // Redirect all of the I/O away from /dev/tty
    fh=open(buf,O_RDWR);
    if (fh<0) { perror(buf);exit(-1);}

    if (S_ISREG(statBuf.st_mode))
    {
	logfileFD=dup(1);
    }
    close(0);close(1); close(2);
    
    dup(fh);dup(fh);dup(fh);
    close(fh);

    // Now, make sure we are not attached to a terminal
    fh=open("/dev/tty",O_RDWR);
    if (fh<0) return; // No terminal
    ioctl(fh,TIOCNOTTY); // Detatch from /dev/tty
    close(fh);
}
// Check to see if the command is really executable:
// Return 0 if the file is executable
int checkCommandFile(const char * command)
{
    int myeuid=geteuid();
    int myegid=getegid();
    struct stat s;
    int ngroups_max=1+sysconf(_SC_NGROUPS_MAX);
    gid_t * groups=new gid_t[ngroups_max];
    ngroups_max=getgroups(ngroups_max,groups);
    int g; // loop iterator 
    mode_t min_permissions=S_IRUSR|S_IXUSR|S_IXGRP|S_IRGRP|S_IROTH|S_IXOTH;

    if (stat(command,&s))
    {
	perror(command);
	return -1;
    }
    if (!S_ISREG(s.st_mode))
    {
	fprintf(stderr,"The command file: %s is not a regular file\n",command);
	return -1;
    }
    if (min_permissions==(s.st_mode & min_permissions)) return 0; // This is great!
    // else
    fprintf(stderr, "Warning- Please change permissions on %s to at least -r-xr-xr-x\n"
    	"procServ may not be able to continue without execute permission!\n",command);
    return 0;
}

connectionItem * connectionItem::head;
// Globals:
time_t procServStart; // Time when this IOC started
time_t IOCStart; // Time when the current IOC was started

/*
 * udpgen2.c
 * a protocol Independant (working for both IPV4 and IPV6) 
 * UDP packets generator
 * by Ph. Jounin, based on udpclient.c written by James R. Binkley
 *
 * OS: Linux
 * To compile: gcc -o udpgen2 -g udpgen2.c
 *
 * udpgen2 sends 'count' packets to a list of destinations
 * if present, the server returns the packet received
 *
 * To run:
 *      1. first run udpserv2 on some udp port
 *      2. then run the client
 *
 * syntax: see syntax()
 *
 */


#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <assert.h>
#include <getopt.h>

#include <pthread.h>

#include <netdb.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

// ----------------------------------------------
// Usage information 
// ----------------------------------------------
void SYNTAX(void)
{
   puts ("udpgen2: a UDP packet generator");
   puts ("syntax:");
   puts ("\t udpgen2 [options] hosts");
   puts ("\t -n, --count=VAL    \t number of packets to be sent");
   puts ("\t -t, --time=VAL     \t test duration");
   puts ("\t -r, --rate=VAL     \t number of packets per second per host");
   puts ("\t -b, --bandwidth=VAL\t target bandwidth per host");
   puts ("\t -l, --length=VAL   \t size of a packet");
   puts ("\t -p, --port=VAL     \t destination port");
   puts ("\t -B, --bind <host>  \t bind to a specific address");
   puts ("\t -V, --verbose      \t detailled output");
exit(0);
} // SYNTAX


enum LogLevels { EMERG, ALERT, CRIT, ERR, WARN, NOTICE, INFO, DEBUG, TRACE };

#define DEFAULT_DURATION 10

// ----------------------------------------------
// CLI parsing informations
// ----------------------------------------------
static struct option longopts [] =
{
   {  "count",       required_argument, NULL, 'n'},
   {  "time",        required_argument, NULL, 't'},
   {  "rate",        required_argument, NULL, 'r'},
   {  "bandwidth",   required_argument, NULL, 'b'},
   {  "length",      required_argument, NULL, 'l'},
   {  "port",        required_argument, NULL, 'p'},
   {  "bind",        required_argument, NULL, 'B'},
   {  "verbose",     no_argument,       NULL, 'V'},
}; // options

struct S_Settings
{
    int pkt_count;		// number
    int duration;		// seconds
    int rate;           // pkt per seconds 
    int pkt_length;
    char *dst_port;
    char *bind_to;
    int verbose;
}
sSettings = { 10, DEFAULT_DURATION, 1, 1024, "54321", NULL, ERR };




// ----------------------------------------------
// per thread information
// ----------------------------------------------
struct S_Host
{
   struct S_Host  *next;
   const char     *name;
   const char     *dst_port;
   // socket info
   int             skt;
   struct sockaddr_storage sa;
   int             sa_len;
   char           *buf;
   int             buflen;
   struct timeval  tv;

   long            count;
   // system info
   pthread_t       thread_id;
}; // S_Host

struct S_Host *pHosts=NULL;


// --------------------------------------------------------
// log function
// --------------------------------------------------------
void LOG (int level, const char *fmt, ...)
{
va_list args;
    if (sSettings.verbose >= level)
    {
         va_start( args, fmt );
         vprintf( fmt, args );
         va_end( args );
    }
} // LOG

// --------------------------------------------------------
// worker thread
// --------------------------------------------------------

// create udp skt and "connect" it 
// in fact, manly resolve the host name
int socket_init (struct S_Host *pHost)
{
struct addrinfo hints, *res;
int rc;

  /*initilize addrinfo structure*/
  memset (&hints, 0, sizeof(struct addrinfo));  
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_DGRAM;
  hints.ai_protocol=IPPROTO_UDP;

  rc = getaddrinfo(pHost->name, pHost->dst_port, &hints, &res);
  if (rc != 0)
    LOG (ERR, "udpclient error for %s, %s: %s", pHost->name, pHost->dst_port, gai_strerror(rc));

  // each of the returned IP address is tried
  do
  {
    pHost->skt = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (pHost->skt>= 0)
      break; /*success*/
  }
  while ((res=res->ai_next) != NULL);

  if (pHost->skt<0)
    LOG (ERR, "can not create socket for host %s\n", pHost->name);

  memcpy (& pHost->sa, res->ai_addr, res->ai_addrlen);
  pHost->sa_len = res->ai_addrlen;

  freeaddrinfo(res);


  rc = connect (pHost->skt, (struct sockaddr *) & pHost->sa, pHost->sa_len);
  if (rc<0) 
    printf ("connect error for host %s\n", pHost->name);

  if (sSettings.verbose) 
      LOG (NOTICE,  "socket %d created for host %s on port %s, %d packets to send\n", 
              pHost->skt, pHost->name, pHost->dst_port, pHost->count);

  // allocate space for datagram
  pHost->buf = malloc (sSettings.pkt_length);
  pHost->buflen = sSettings.pkt_length;
return rc;
} // socket_init


// -------------
// the sender thread 
// -------------
#define RATE_MON 50
void *UdpBulkSend (void *pVoid)
{
struct S_Host *pHost = (struct S_Host *) pVoid;
struct timeval tv, before, after, before100, after100, reference;
         
   gettimeofday (& after, NULL) ;
   gettimeofday (& after100, NULL);

   reference = pHost->tv;
   for ( ; pHost->count > 0 ;  )
   { 
  	    gettimeofday (& before, NULL) ; 
		send (  pHost->skt,
						 pHost->buf,
						 pHost->buflen,
						 0 );
         // do not process error, since we want send packets
         // regardless if the sender is listening or not 

         LOG (TRACE, "packet #%d sent to %s\n", pHost->count, pHost->name);
         pHost->count--;
	     
         tv = reference;

	     gettimeofday (& after, NULL) ;
		 if (after.tv_usec > before.tv_usec)
				 tv.tv_usec = reference.tv_usec - (after.tv_usec - before.tv_usec);
		 if (tv.tv_usec>0) 
				select ( 0, NULL, NULL, NULL, & tv );
		
		 if (pHost->count % RATE_MON == 0)
		 {long tm; int retard;
		     gettimeofday (& before100, NULL);
		     tm =  (before100.tv_sec - after100.tv_sec) * 1000000 + before100.tv_usec - after100.tv_usec ;
		     retard = (tm - pHost->tv.tv_usec * RATE_MON) * RATE_MON / tm;
 //if (retard<0) printf ("tm %ld, %ld, retard %d packets\n", tm, (long) pHost->tv.tv_usec * RATE_MON, retard);
 if (retard >  RATE_MON/4 ) reference.tv_usec--;
 if (retard < -RATE_MON/8 ) reference.tv_usec++;
			  for ( ; retard > 0; retard --) 
			  {
				 send (  pHost->skt,
						 pHost->buf,
						 pHost->buflen,
						 0 );
			     pHost->count --;
			  }
		     gettimeofday (& after100, NULL);
		 }
    }
printf ("end with latency %d\n", reference.tv_usec);
    close (pHost->skt);
    return NULL;
} // UdpBulkSend




// --------------------------------------------------------
// parse arguments
// --------------------------------------------------------

// Add Host to the list (keep same order => add at the end of the list)
void PushHost (const char *name, const char *port, 
               int rate, int count )
{
struct S_Host *pNewHost;
   pNewHost = calloc ( sizeof *pNewHost, 1 );
   pNewHost->next = NULL;

   // payload
   pNewHost->name = name;
   pNewHost->dst_port = port;
   pNewHost->skt = -1;
   // keep only count and set the delay                  
   pNewHost->tv.tv_sec  = 1 / rate;
   pNewHost->tv.tv_usec = (int) (1000000.0 / rate);
   pNewHost->count = count;


   LOG (INFO, "#%d packets to send to host %s, port %s, (delay %d s/%d us, rate %d)\n", 
               pNewHost->count, pNewHost->name, pNewHost->dst_port, 
               pNewHost->tv.tv_sec, pNewHost->tv.tv_usec, rate);

   // add the item at the end of the list
   if (pHosts == NULL) pHosts = pNewHost;
   else
   {struct S_Host *pLast;
       for (pLast = pHosts ; pLast->next!=NULL ; pLast = pLast->next);
       pLast->next = pNewHost;
   } 
} // PushHost


// perform a atoi translation, but allow k,m,g,K,M,G suffixes
// code from iperf3-7
unsigned long atoi_suffix (const char *s)
{
double    n;
char      suffix = '\0';
const double KILO_UNIT = 1024.0;
const double MEGA_UNIT = 1024.0 * 1024.0;
const double GIGA_UNIT = 1024.0 * 1024.0 * 1024.0;
const double TERA_UNIT = 1024.0 * 1024.0 * 1024.0 * 1024.0;

  
     assert(s != NULL);
     /* scan the number and any suffices */
     sscanf(s, "%lf%c", &n, &suffix);
     /* convert according to [Tt Gg Mm Kk] */
     switch    (suffix)
     {
         case 't': case 'T':
              n *= TERA_UNIT; break;
         case 'g': case 'G':
              n *= GIGA_UNIT; break;
         case 'm': case 'M':
              n *= MEGA_UNIT; break;
         case 'k': case 'K':
              n *= KILO_UNIT; break;
         default:
             break;
      }
      return (unsigned long) n;
} // atoi_suffix


int parse_arguments (int argc, char *argv[])
{
int flag;
int bandwidth=-1, pkt_count=-1, rate = -1, duration=-1;
   while ((flag = getopt_long(argc, argv, "n:t:r:b:l:p:B:V", longopts, NULL)) !=-1)
   {
       switch (flag)
       {
          case 'n' : pkt_count = atoi_suffix(optarg);
                     break;
          case 't' : duration  = atoi(optarg);
                     break;
          case 'r' : rate = atoi_suffix(optarg);
                     break;
          case 'b' : bandwidth =  atoi_suffix(optarg);
                     break;
          case 'l' : sSettings.pkt_length = atoi_suffix(optarg);
                     break;
          case 'p' : sSettings.dst_port = optarg;
                     break;
          case 'B' : sSettings.bind_to = optarg;
                     break;
          case 'V' : sSettings.verbose++;
                     break;
          default :
	             SYNTAX();
       } // parse flag argument
   } // parse all options

   // rate & bandwidth are redundant, pkt_count and duration too
   // --> keep only rate and pkt_count
   if (bandwidth!=-1)    
  	     rate = bandwidth / sSettings.pkt_length / 8;
   if (rate==-1)
         rate = (pkt_count==-1 ? sSettings.pkt_count : pkt_count) / (duration==-1 ? sSettings.duration : duration);
   // rate is fixed 
   if (pkt_count==-1)  
              pkt_count = (duration==-1 ? sSettings.duration : duration) * rate;  // rate changed, not packet
   // trailing arguments --> Hosts
   while (optind < argc )
      PushHost ( argv[optind++],                  sSettings.dst_port, 
                 rate,
                 pkt_count );
  
   // check parameter quality
   if (pHosts==NULL) SYNTAX();
   return argc;
} // parse_arguments



  
// --------------------------------------------------------
// main routine : 
// call parse_argument
// start a thread per host and wait for them
// --------------------------------------------------------
int main (int argc, char *argv[])
{
struct S_Host  *pHost;
int             rc; 
void           *res;

  parse_arguments (argc, argv);   // fill sSettings structure and pHosts linked list 

  // ignore SIGPIPE signal (socket closed), avoid to terminate main thread !!
  signal(SIGPIPE, SIG_IGN);

   // init all sockets
   for (pHost=pHosts ; pHost!=NULL ; pHost = pHost->next)
       socket_init (pHost);

   // start all threads
   for (pHost=pHosts ; pHost!=NULL ; pHost = pHost->next)
       rc =   pthread_create (& pHost->thread_id, NULL, UdpBulkSend, pHost);
   if (rc) LOG(ERR, "can not create thread\n");
sleep (5);
   // wait for all threads to terminate
    for (pHost=pHosts ; pHost!=NULL ; pHost = pHost->next)
        rc = pthread_join (pHost->thread_id, &res);

   // print report 
} //main


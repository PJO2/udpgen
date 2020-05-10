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


// Log levels
enum LogLevels { EMERG, ALERT, CRIT, ERR, WARN, NOTICE, INFO, DEBUG, TRACE };

#define DEFAULT_DURATION 10

// recalibrate delay each 50 packets
#define CALIBRATE_FREQ 50


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
   const char     *name;		// destination
   const char     *dst_port;	
   // socket info
   int             skt;
   struct sockaddr_storage sa;	// sockaddr
   int             sa_len;		// sockaddr_len
   char           *buf;			// frame to send
   int             buflen;		// frame size
   time_t          us_between_packets;		// delay	
   
   size_t          count;		// # packet to send
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
// microseconds since program start
// --------------------------------------------------------

time_t _microseconds ()
{
static time_t start_time=0; 
struct timeval now;    

    gettimeofday (& now, NULL) ; 
    if (start_time == 0)  //
    {
		start_time = now.tv_sec * 1000000 + now.tv_usec;
		return 0 ;
    }
return now.tv_sec * 1000000 + now.tv_usec - start_time;
} // _microseconds


// --------------------------------------------------------
// network functions
// --------------------------------------------------------

// --------
// create udp skt and "connect" it 
// in fact, manly resolve the host name
// --------
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
     LOG (ERR, "connect error for host %s\n", pHost->name);
  LOG (NOTICE, "socket %d created for host %s on port %s, %d packets to send\n", 
               pHost->skt, pHost->name, pHost->dst_port, pHost->count);

  // allocate space for datagram
  pHost->buf = malloc (sSettings.pkt_length);
  pHost->buflen = sSettings.pkt_length;
return rc;
} // socket_init


// -----------
// the sender thread 
// -----------
void *UdpBulkSend (void *pVoid)
{
struct S_Host *pHost = (struct S_Host *) pVoid;
time_t         us_op_delay, before_send, before_calibration=0 ; 
struct timeval tv;
           
         
   // us_delay may can
   us_op_delay = pHost->us_between_packets;  // keep us_between_packets as reference
   
   for ( ; pHost->count > 0 ;  pHost->count-- )
   { 
	    before_send = _microseconds ();
	    // send the packet do not retrieve error code
		send (  pHost->skt, pHost->buf, pHost->buflen, 0 );
        LOG (TRACE, "packet #%d sent to %s\n", pHost->count, pHost->name);
        
		// wait estimated delay
		tv.tv_sec = 0;
		tv.tv_usec = us_op_delay - (_microseconds () - before_send); 
		select (1, NULL, NULL, NULL, & tv);
		
		// now each 50 packets, we compare the effective rate to the target rate (pHost->us_delay_betw..)
		// and adjust the operationnal rate (us_op_delay)
		if (pHost->count % CALIBRATE_FREQ == 0)
		{time_t us_late; 
	     signed long pkts_late;
		     if (before_calibration!=0)
		     {
				 us_late =    ( _microseconds() - before_calibration )       // time spent to send 50 packets
							-  pHost->us_between_packets * CALIBRATE_FREQ ;  // target time to send 50 pkts
				 pkts_late =  us_late / pHost->us_between_packets ;			 // number of packet late
				 
				 // correct the delay 
				 if (pkts_late >  CALIBRATE_FREQ/4 )
				 {
				      us_op_delay--;		  // we are late, decrease delay
				 	  before_calibration = 0; // and do not use this set for calibration
                 }
				 if (pkts_late < -CALIBRATE_FREQ/8 )
				 {
					  us_op_delay++;		// we are in advance, increase delay
				 	  before_calibration = 0; // and do not use this set for calibration
                 }
				 // and send immediately the missing packets
				 for ( ; pkts_late > 0; pkts_late --) 
				  {
					 send (  pHost->skt, pHost->buf, pHost->buflen, 0 );
					 pHost->count --;
				  }
		     }
		     before_calibration = _microseconds();	// for next calibration
		 }
    }
    LOG (INFO, "end with latency %d\n", us_op_delay);
    
    close (pHost->skt);
    return NULL;
} // UdpBulkSend




// --------------------------------------------------------
// parsing arguments
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
   pNewHost->us_between_packets= (long) (1000000.0 / rate);
   pNewHost->count = count;

   LOG (INFO, "#%d packets to send to host %s, port %s, (delay %d us, rate %d)\n", 
               pNewHost->count, pNewHost->name, pNewHost->dst_port, 
               pNewHost->us_between_packets, rate);

   // add the record at the end of the list
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

// -------
// parse arguments using getopt_long
// -------
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
      PushHost ( argv[optind++],
                 sSettings.dst_port, 
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

   // wait for all threads to terminate
    for (pHost=pHosts ; pHost!=NULL ; pHost = pHost->next)
        rc = pthread_join (pHost->thread_id, &res);

   // print report 
} //main


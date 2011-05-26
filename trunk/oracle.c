/* this file gathers information from neighbour nodes in order
 * to estimate the topology of the network.
 *
 * All nodes transmit their geographical positions, as well 
 * as the positions of all nodes which they have had contact with (ever). 
 * This transmission takes place on a regular timed interval.
 * 
 * Nodes also transmit their own available public buffer space,
 * but do not retransmit this information.
 *
 * Routing is non-flooding, packets are forwarded only once.
 *
 * Packets are routed to any node which is closer than itself to the
 * last known position of the destination node. If no position
 * is known for the destination node, the packet is not forwarded.
 * The exception being, if a neighbour node's buffers are full,
 * then the packet will not be forwarded to that neighbour.
 *
 */
#include "dtn.h"
#include <stdlib.h>

/* structure to represent node and location */
typedef struct 
{
	CnetAddr addr;
	CnetPosition loc;
	uint32_t timestamp; /* time when this location was seen, given in 
							the local time of the sender, in SECONDS */
						/* It's not necessary to try synchronise this
						 * because we'll only be comparing it to timestamps
						 * from the very same sender */
} NODELOCATION;

#define ORACLE_HEADER_SIZE (sizeof(NODELOCATION) + sizeof(uint32_t)*3)
#define MAX_ORACLE_PAYLOAD (MAX_PACKET_SIZE - ORACLE_HEADER_SIZE)

/* the packet structure for oracle information transmission */
typedef struct 
{
	uint32_t checksum; /*crc32 checksum of the oraclepacket including 'locations' payload */
	NODELOCATION senderLocation;
	uint32_t freeBufferSpace; /* how many bytes of space available in transmitting nodes' public buffer */
	uint32_t locationsSize; /* how many elements in locations */
	NODELOCATION locations[MAX_ORACLE_PAYLOAD]; /* array of (last known) locations of known hosts */	
} OraclePacket;

/* structure to store information about neighbours */
/* TODO: store distance to this neighbour here to speed up computation */
typedef struct 
{
	NODELOCATION nl;
	uint32_t freeBufferSpace;
	uint64_t lastBeacon; /* when did we last see a bacon from this noodle */
} Neighbour;

//last known addresses for all known nodes
static Neighbour * positionDB;
static int dbsize;

static int compareNL(const void * key, const void * elem) 
{
	uint32_t k = *((uint32_t *)key);
	uint32_t addr = (uint32_t)(((Neighbour *)(elem))->nl.addr);
	return k-addr ;
}

/* add a position to the positionDB
 * maintaining db sortedness
 * or update the existing position if the address
 * already exists
 */
static void savePosition(NODELOCATION n) 
{
	CnetAddr * np = &(n.addr);
	Neighbour * nbp  = bsearch(np, positionDB, dbsize, sizeof(Neighbour), compareNL);
	if(nbp == NULL) 
	{
		//append and sort
		dbsize++;
		positionDB = realloc(positionDB, sizeof(Neighbour)*dbsize);
		positionDB[dbsize-1].nl = n;
		positionDB[dbsize-1].lastBeacon = 0; 
		qsort(positionDB, dbsize, sizeof(Neighbour), compareNL);
	} 
	else 
	{
		//update location for this node if it's a newer reading
		if(nbp->nl.timestamp < n.timestamp) {
			nbp->nl.loc = n.loc;
			nbp->nl.timestamp = n.timestamp;
			printf("Node %d Oracle: Updated the location of node %d to %d,%d from time %d\n", nodeinfo.nodenumber, n.addr, n.loc.x, n.loc.y, n.timestamp);
		}
	}
}

/* checksum an oracle packet, return the result
 * crc32
 */
static uint32_t checksum_oracle_packet(OraclePacket * p) 
{
	p->checksum = 0;
	return CNET_crc32((unsigned char*)p, sizeof(OraclePacket) - sizeof(p->locations) + sizeof(NODELOCATION)*p->locationsSize);
	//return 0;
}

/* remove a specific item from the DB
 */
static void dbRemove(int index) {
	if(index != dbsize-1) {
		for(int i=index+1;i<dbsize;i++) {
			positionDB[i-1] = positionDB[i]; /* shift entries */	
		}
	}
	dbsize--;
	positionDB = realloc(positionDB, sizeof(Neighbour)*dbsize);
		
}

/* prune oldest entry from the local db
 * only invoked in edge cases
 */
static void pruneOldestRecord() {
	uint64_t oldest = -1;
	int oldestIndex = -1;
	for(int i=0;i<dbsize;i++) {
		if(positionDB[i].lastBeacon < oldest || oldestIndex == -1) {
			oldest = positionDB[i].lastBeacon;
			oldestIndex = i;
		}
	}
	dbRemove(oldestIndex);
}

/* prune old info from the db if it is overfull
 * no need to optimise this will only be invoked in edge cases
 */
static void pruneDB() {
	int n = dbsize - (MAX_ORACLE_PAYLOAD / sizeof(NODELOCATION));
	for(int i=0;i<n;i++) {
		pruneOldestRecord();
	}
}

/* broadcast info about this node and other known nodes
 */
EVENT_HANDLER(sendOracleBeacon)
{
	/* possible todo: restructure the Neighbour data so that this can be done with one memcpy */
	/*printf("Node %d: starting beacon send\n", nodeinfo.nodenumber);*/
	OraclePacket p;
	
	/* if cant send our DB in one beacon, then prune the DB */
	if(dbsize > (MAX_ORACLE_PAYLOAD / sizeof(NODELOCATION))) {
		pruneDB();
	}

	/* copy our db to the packet */
	for(int i=0;i<dbsize;i++) 
	{
		p.locations[i] = positionDB[i].nl;
	}
	p.freeBufferSpace = get_public_nbytes_free();
	p.locationsSize = dbsize;
	p.senderLocation.addr = nodeinfo.nodenumber;
	CnetPosition loc;
	CNET_get_position(&loc, NULL);
	p.senderLocation.loc = loc;
	p.senderLocation.timestamp = nodeinfo.time_in_usec/1000000;
	printf("My time is %ds\n", nodeinfo.time_in_usec/1000000);
	char * pp = (char *)(&(p));	
	p.checksum = checksum_oracle_packet(&p);
	int len = sizeof(p) - sizeof(p.locations) + sizeof(NODELOCATION)*dbsize;
	assert(len < MAX_PACKET_SIZE);
	link_send_info(pp, len, ALLNODES);
	printf("Node %d: sent beacon to link\n", nodeinfo.nodenumber);
	/* send again later */
	CNET_start_timer(EV_TIMER7, (CnetTime)ORACLEINTERVAL, 0);
}

/* process an oracle packet and update local knowledge
 * database
 */
static void processBeacon(OraclePacket * p) 
{
	for(int i=0;i < p->locationsSize;i++) 
	{
		if((int)p->locations[i].addr != (int)nodeinfo.nodenumber) /* if not THIS node */
			savePosition(p->locations[i]);
	}

	/* save some near-neighbour specific info */
	savePosition(p->senderLocation);
	Neighbour * nbp = bsearch(&(p->senderLocation.addr), positionDB, dbsize, sizeof(Neighbour), compareNL);
	nbp->lastBeacon = nodeinfo.time_in_usec; 
	nbp->freeBufferSpace = p->freeBufferSpace;
}

/* find the last known position of a node (a), sets l
 * equal to the last known position
 * returns false if unknown
 */
static bool queryPosition(CnetPosition * l, CnetAddr a) 
{
	Neighbour * nbp = bsearch(&a, positionDB, dbsize, sizeof(Neighbour), compareNL);
	if(nbp==NULL) 
	{
		return false;
	} 
	else 
	{
		*l = nbp->nl.loc;
		return true;
	}
}

/* returns true iff: 
 * 	a->c > b->c
 * 	by some interval defined in dtn.h
 */
bool isCloser(CnetPosition a, CnetPosition b, CnetPosition c, int interval) 
{
	int cax = c.x - a.x;
	int cay = c.y - a.y;
	int cbx = c.x - b.x;
	int cby = c.y - b.y;
	if( cbx*cbx + cby*cby + interval*interval < cax*cax + cay*cay )
		return true;
	else 
		return false;
}

/* Sets ptr to the best intermediate node by which a messag
 * to dest should be delivered.
 * n is indexed from 0
 *
 * Returns false if there is no best node (i.e. it is best to
 * buffer the message), otherwise returns true
 *
 * This function will only 'recommend' a node which is in range and
 * has buffer space to transmit. 
 *
 * Note: function will currently recommend broadcasting to ANY node
 * which causes an improvement, rather than greedily trying to find
 * the BEST node.
 *
 */
bool get_nth_best_node(CnetAddr * ptr, int n, CnetAddr dest, size_t message_size) 
{
	printf("Node %d Oracle: Hunting for a route to %d...\n", nodeinfo.nodenumber, dest);
	CnetTime t = nodeinfo.time_in_usec;
	CnetPosition destPos;  
	if(!queryPosition(&destPos, dest)) {
		printf("Node %d Oracle: I've never heard of %d!\n", nodeinfo.nodenumber, dest);
		return false;
	}
	if(n!=0) return false;
	else 
	{
		for(int i=0; i<dbsize;i++) 
		{
			if(t > positionDB[i].lastBeacon + ORACLEWAIT ) continue; /* skip this neighbour if we haven't had a beacon from it recently */ 
			if((int)positionDB[i].freeBufferSpace < message_size) continue; /* enough buffer space for this massage */
			CnetPosition nextPos = positionDB[i].nl.loc;
			CnetPosition myPos; CNET_get_position(&myPos, NULL);	
			if( positionDB[i].nl.addr == dest ) {	
				*ptr = positionDB[i].nl.addr;
				printf("Node %d Oracle: %d is the target, and in range!\n", nodeinfo.nodenumber, *ptr);
				return true;
			} else if( isCloser(myPos, nextPos, destPos, MINDIST) ) 
			{
				*ptr = positionDB[i].nl.addr;
				printf("Node %d Oracle: I think %d is closer to %d than us\n", nodeinfo.nodenumber, *ptr, (int)dest);
				return *ptr != nodeinfo.nodenumber;
			}
		}
	}
	printf("Node %d Oracle: I can't find anyone closer to %d than us!\n", nodeinfo.nodenumber, (int)dest);
	return false;

}

/* Messages from other nodes which use link_send_info will
 * be passed up to here from the data link layer 
 */
void oracle_recv(char * msg, int len, CnetAddr rcv) 
{
	/* parse info from other nodes to estimate topology */
	OraclePacket * p = (OraclePacket *) msg;
	uint32_t oldsum = p->checksum;
	if(p->locationsSize*sizeof(NODELOCATION) + ORACLE_HEADER_SIZE != len) {
		printf("got a corrupt oracle packet\n");
		return; /* corrupt length */
	}
	if(checksum_oracle_packet(p)==oldsum) 
	{
		printf("Node %d oracle: Got an oracle packet from %d; contents:\n", nodeinfo.nodenumber, p->senderLocation.addr);
		printf("\tNode %d is at %d,%d\n", p->senderLocation.addr, p->senderLocation.loc.x, p->senderLocation.loc.y);
		for(int i=0;i<p->locationsSize;i++) {
			printf("\tNode %d spotted @ %d,%d ; %d oclock\n", p->locations[i].addr, p->locations[i].loc.x, p->locations[i].loc.y, p->locations[i].timestamp);
		}
		processBeacon(p);
	}
	net_send_buffered();
}

/* function is called on program intialisation 
 */
void oracle_init() 
{
	dbsize = 0;
	positionDB = NULL;

	CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);
	/* schedule periodic transmission of topology information, or whatever */		
	CNET_set_handler(EV_TIMER7, sendOracleBeacon, 0);
	//CNET_start_timer(EV_TIMER7, (CnetTime)((CNET_rand() % ORACLEINTERVAL)), 0);
	CNET_start_timer(EV_TIMER7, (CnetTime)(1000), 0);
}

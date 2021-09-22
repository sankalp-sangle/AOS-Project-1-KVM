#include<stdio.h>
#include<stdlib.h>
#include<libvirt/libvirt.h>
#include<math.h>
#include<string.h>
#include<unistd.h>
#include<limits.h>
#include<signal.h>
#define MIN(a,b) ((a)<(b)?a:b)
#define MAX(a,b) ((a)>(b)?a:b)
#define FACTOR 1024

// Note : ensure wastage threshold is 2 * starvation threshold
#define ABSOLUTE_LOWER_BOUND 100
#define ABSOLUTE_UPPER_BOUND 300
#define STARVATION_RATIO 0.2
#define WASTAGE_RATIO 0.4

int is_exit = 0; // DO NOT MODIFY THE VARIABLE

unsigned long host_free_memory = 0;
int total_domains = 0;
virDomainPtr *domainsList;
int* countOfExcessCycles;

void MemoryScheduler(virConnectPtr conn,int interval);

typedef struct starveNode {
	int id;
	unsigned long actual;
	unsigned long available;
	unsigned long unused;
	double usageratio;
	int lowerLimit;
	struct starveNode* next;

} starveNode;

typedef struct wasteNode {
	int id;
	unsigned long actual;
	unsigned long available;
	unsigned long unused;
	double usageratio;
	int upperLimit;
	struct wasteNode* next;
} wasteNode;

starveNode* addStarveNode(starveNode* head, starveNode* node) {
	if(head == NULL) {
		return node;
	}
	starveNode* tmp = head;
	while(head->next) {
		head = head->next;
	}
	head->next = node;
	return tmp;
}

wasteNode* addWasteNode(wasteNode* head, wasteNode* node) {
	if(head == NULL) {
		return node;
	}
	wasteNode* tmp = head;
	while(head->next) {
		head = head->next;
	}
	head->next = node;
	return tmp;
}

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
void signal_callback_handler()
{
	printf("Caught Signal");
	is_exit = 1;
}

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
int main(int argc, char *argv[])
{
	virConnectPtr conn;

	if(argc != 2)
	{
		printf("Incorrect number of arguments\n");
		return 0;
	}

	// Gets the interval passes as a command line argument and sets it as the STATS_PERIOD for collection of balloon memory statistics of the domains
	int interval = atoi(argv[1]);
	
	conn = virConnectOpen("qemu:///system");
	if(conn == NULL)
	{
		fprintf(stderr, "Failed to open connection\n");
		return 1;
	}

	signal(SIGINT, signal_callback_handler);



	unsigned int flags = VIR_CONNECT_LIST_DOMAINS_RUNNING |
						VIR_CONNECT_LIST_DOMAINS_PERSISTENT;
	total_domains = virConnectListAllDomains(conn, &domainsList, flags);
	if (total_domains < 0) {
		printf("Error in call to virConnectListAllDomains\n");
		exit(0);
	}

	countOfExcessCycles = malloc(sizeof(int) * total_domains);

	for(int i = 0; i < total_domains; i++) {
		int ret = virDomainSetMemoryStatsPeriod(domainsList[i], interval, VIR_DOMAIN_AFFECT_CURRENT);
		if(ret == -1) {
			printf("Failure in virDomainSetMemorStatsPeriod call\n");
			exit(0);
		}
		countOfExcessCycles[i] = 0;
	}

	

	while(!is_exit)
	{
		// Calls the MemoryScheduler function after every 'interval' seconds
		MemoryScheduler(conn, interval);
		sleep(interval);
	}

	// Close the connection
	virConnectClose(conn);
	return 0;
}

/*
COMPLETE THE IMPLEMENTATION
*/
void MemoryScheduler(virConnectPtr conn, int interval)
{
	printf("+++++++++++++++++++++++++++++++++++++++\n");
	int nparams = 0;
	virNodeMemoryStatsPtr params;
	if (virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, NULL, &nparams, 0) == 0 && nparams != 0) {
		params = malloc(sizeof(virNodeMemoryStats) * nparams);
		memset(params, 0, sizeof(virNodeMemoryStats) * nparams);
		virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, params, &nparams, 0);
	}

	for (int i=0; i < nparams; i++)
	{
		if(strcmp(params[i].field, "free") == 0) {
			host_free_memory = params[i].value;
			printf("Host %s memory %lu mb\n", params[i].field, host_free_memory / FACTOR);
		}
	}

	starveNode* headStarve = NULL;
	int countStarve = 0;
	wasteNode* headWaste = NULL;
	int countWaste = 0;


	for (int i = 0; i < total_domains; i++) {
		virDomainMemoryStatStruct mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];

		if (virDomainMemoryStats(domainsList[i], mem_stats, VIR_DOMAIN_MEMORY_STAT_NR, 0) == -1) {
			printf("Error in call to virDomainMemoryStats\n");
			exit(0);
		}

		unsigned long long actual;
		unsigned long long available;
		unsigned long long unused;
		for (int j = 0; j < VIR_DOMAIN_MEMORY_STAT_NR; j++) {
			if (mem_stats[j].tag == VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON) {
				actual = mem_stats[j].val;
			} else if (mem_stats[j].tag == VIR_DOMAIN_MEMORY_STAT_AVAILABLE) {
				available = mem_stats[j].val;
			} else if (mem_stats[j].tag == VIR_DOMAIN_MEMORY_STAT_UNUSED) {
				unused = mem_stats[j].val;
			}

		}
		printf("%s: actual %lluMB; unused--%lluMB. Ratio: %f\n", virDomainGetName(domainsList[i]),
				actual / FACTOR,
				unused / FACTOR,
				(double) unused / actual);

		if( unused < STARVATION_RATIO * actual  || unused < ABSOLUTE_LOWER_BOUND * FACTOR) {
			starveNode* snode = malloc(sizeof(starveNode));
			snode->actual = actual;
			snode->available = available;
			snode->unused = unused;
			snode->id = i;
			snode->usageratio = 1.0 * unused / actual;
			snode->next = NULL;
			headStarve = addStarveNode(headStarve, snode);
			countStarve++;
			countOfExcessCycles[i] = 0;
			snode->lowerLimit = 0;
			if(unused < ABSOLUTE_LOWER_BOUND * FACTOR) {
				snode->lowerLimit = 1;
			}
		} else if(unused >= WASTAGE_RATIO * actual || unused > ABSOLUTE_UPPER_BOUND * FACTOR) {
			wasteNode* wnode = malloc(sizeof(wasteNode));
			wnode->actual = actual;
			wnode->available = available;
			wnode->unused = unused;
			wnode->id = i;
			wnode->usageratio = 1.0 * unused / actual;
			wnode->next = NULL;
			headWaste = addWasteNode(headWaste, wnode);
			countWaste++;
			countOfExcessCycles[i] += 1;
			wnode->upperLimit = 0;
			if(unused > ABSOLUTE_UPPER_BOUND * FACTOR) {
				wnode->upperLimit = 1;
			}
		} else {
			countOfExcessCycles[i] = 0;
		}	
	}

	printf("Starve Nodes: %d\n", countStarve);
	printf("Waste Nodes: %d\n", countWaste);
	// int res = virDomainSetMemory(domainsList[curr->id], 2 * curr->actual);
	if(countStarve > 0) {
		starveNode* curr = headStarve;
		while(curr) {
			if(curr->lowerLimit) {
				printf("Lower limit got reached for %s, hence allocating a bonus of 100 MB\n", virDomainGetName(domainsList[curr->id]));
				if(100 * FACTOR > host_free_memory) {
					printf("Cannot give from host atleast\n");
					curr = curr->next;
					continue;
				}
				int res = virDomainSetMemory(domainsList[curr->id], curr->actual + 100 * FACTOR);
				curr = curr->next;
				continue;
			}
			unsigned long allocation = ( ( 3 * curr->actual / 10 - curr->unused )  * 10) / 7;
			printf("Ratio is : %f \n", curr->usageratio);
			printf("Allocation is : %lu \n", allocation);
			printf("Actual is : %lu \n", curr->actual);
			printf("Unused is : %lu \n", curr->unused);
			if(allocation < host_free_memory) {
				printf("Allocation is less than host free memory\n");
				int res = virDomainSetMemory(domainsList[curr->id], curr->actual + allocation);
				printf("Allocating extra memory of %lu for domain %s\n", allocation, virDomainGetName(domainsList[curr->id]));
				host_free_memory -= allocation;
			} else {
				printf("Cannot provide more memory from the host atleast\n");
				printf("Allocation asked was : %lu \n", allocation);
				printf("Host free memory is : %lu \n", host_free_memory);
			}
			curr = curr->next;
		}
	}
	if(countWaste > 0) {
		wasteNode* curr = headWaste;
		while(curr) {
			if(countOfExcessCycles[curr->id] >= 3) { // Third time excess, take away memory
				unsigned long takeaway = (curr->unused - 3 * curr->actual / 10) * 10 / 7;
				if(takeaway > 300 * FACTOR) {
					int res = virDomainSetMemory(domainsList[curr->id], curr->actual - 250 * FACTOR);
					printf("Took away extra memory of 250 for domain %s as takeaway was too large\n", virDomainGetName(domainsList[curr->id]));
					countOfExcessCycles[curr->id] = 0;
					curr = curr->next;
					continue;
				}
				if(curr->upperLimit) {
					printf("Upper limit got reached for %s, hence withdrawing a memory of 100 MB\n", virDomainGetName(domainsList[curr->id]));
					int res = virDomainSetMemory(domainsList[curr->id], curr->actual - 100 * FACTOR);
					curr = curr->next;
					continue;
				}
				
				if(curr->actual - takeaway <= 120) {
					int res = virDomainSetMemory(domainsList[curr->id], 120);
					printf("Took away extra memory and kept total equals 120 for domain %s\n", virDomainGetName(domainsList[curr->id]));
				} 
				else {
					int res = virDomainSetMemory(domainsList[curr->id], curr->actual - takeaway);
					printf("Took away extra memory of %lu for domain %s\n", takeaway / FACTOR, virDomainGetName(domainsList[curr->id]));
					host_free_memory += takeaway;
				}
			}
			curr = curr->next;
		}
	}
}

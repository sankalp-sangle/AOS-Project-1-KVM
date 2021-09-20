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
#define STARVATION_THRESHOLD 150000
#define WASTAGE_THRESHOLD 300000

int is_exit = 0; // DO NOT MODIFY THE VARIABLE

unsigned long long host_free_memory = 0;
int total_domains = 0;
virDomainPtr *domainsList;

void MemoryScheduler(virConnectPtr conn,int interval);

typedef struct starveNode {
	int id;
	unsigned long long actual;
	unsigned long long available;
	unsigned long long unused;
	struct starveNode* next;
} starveNode;

typedef struct wasteNode {
	int id;
	unsigned long long actual;
	unsigned long long available;
	unsigned long long unused;
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

	for(int i = 0; i < total_domains; i++) {
		int ret = virDomainSetMemoryStatsPeriod(domainsList[i], interval, VIR_DOMAIN_AFFECT_CURRENT);
		if(ret == -1) {
			printf("Failure in virDomainSetMemorStatsPeriod call\n");
			exit(0);
		}
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
	int nparams = 0;
	virNodeMemoryStatsPtr params;
	if (virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, NULL, &nparams, 0) == 0 && nparams != 0) {
		params = malloc(sizeof(virNodeMemoryStats) * nparams);
		memset(params, 0, sizeof(virNodeMemoryStats) * nparams);
		virNodeGetMemoryStats(conn, VIR_NODE_MEMORY_STATS_ALL_CELLS, params, &nparams, 0);

		for (int i=0; i< nparams; i++)
		{
			if(strcmp(params[i].field, "free") == 0) {
				host_free_memory = params[i].value / FACTOR;
				printf("Host %s memory %llu\n", params[i].field, host_free_memory);
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
			printf("%s: actual balloon size--%lluMB;\tavailable--%lluMB;\tunused--%lluMB.\n", virDomainGetName(domainsList[i]),
					actual / FACTOR,
					available / FACTOR,
					unused / FACTOR);

			if(unused < STARVATION_THRESHOLD) {
				starveNode* snode = malloc(sizeof(starveNode));
				snode->actual = actual;
				snode->available = available;
				snode->unused = unused;
				snode->id = i;
				snode->next = NULL;
				headStarve = addStarveNode(headStarve, snode);
				countStarve++;
			} else if(unused >= WASTAGE_THRESHOLD) {
				wasteNode* wnode = malloc(sizeof(wasteNode));
				wnode->actual = actual;
				wnode->available = available;
				wnode->unused = unused;
				wnode->id = i;
				wnode->next = NULL;
				headWaste = addWasteNode(headWaste, wnode);
				countWaste++;
			}	
		}

		printf("Starve Nodes: %d\n", countStarve);
		printf("Waste Nodes: %d\n", countWaste);

		if(headStarve == NULL) {
			printf("No need to coordinate anything!\n");
			// if(headWaste == NULL) {
			// 	printf("returning\n");
			// 	return;
			// } else {
			// 	wasteNode* curr = headWaste;
			// 	while(curr) {
			// 		printf("Here\n");
			// 		int res = virDomainSetMemory(domainsList[curr->id], curr->actual - 3 * STARVATION_THRESHOLD / 10);
			// 		if(res == 0) printf("Succes\n");
			// 		else printf("Failure!\n");
			// 		printf("Setting memory for %s from %llu to %llu\n", virDomainGetName(domainsList[curr->id]), curr->actual, curr->actual - 3 * STARVATION_THRESHOLD / 10);
			// 		curr = curr->next;
			// 	}
			// }
		} else {
			starveNode* curr = headStarve;
			while(curr) {
				// if(headWaste != NULL) {
				// 	int res = virDomainSetMemory(domainsList[headWaste->id], headWaste->actual - 3 * STARVATION_THRESHOLD / 10);
				// 	if(res == 0) printf("At line 226\n");
				// 	else printf("Failure!\n");
				// 	printf("Setting memory for %s from %llu to %llu\n", virDomainGetName(domainsList[headWaste->id]), headWaste->actual, headWaste->actual - 3 * STARVATION_THRESHOLD / 10);
				// 	headWaste = headWaste->next;

				// } else {

				// }
				int res = virDomainSetMemory(domainsList[curr->id], 2 * curr->actual);
				curr = curr->next;
			}
		}

		

	}
}

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
#define DIV_FACTOR 1000

int is_exit = 0; // DO NOT MODIFY THIS VARIABLE

int total_pcpus = 0;
int total_vcpus = 0;
int firstIteration = 1;

unsigned long long int* vcpuUsage; // Stores the per vcpu cpuTime (from the start)
unsigned long long int* pcpuUsage; // Stores the per pCPU cpuTime (for the last interval)


typedef struct vcpuNode {
	double usage;
	int id;
} vcpuNode;

vcpuNode* vcpuarray; // Stores the per vcpu cpuTime for the last interval

void CPUScheduler(virConnectPtr conn,int interval);

unsigned char* generateMap(int total_pcpus, int cpuIndex) {
	unsigned char* map = malloc(VIR_CPU_MAPLEN(total_pcpus));
	memset(map, 0, VIR_CPU_MAPLEN(total_pcpus));
	unsigned char* ptr = map;
	int offset = cpuIndex / 8;
	ptr += offset;
	*ptr = (0x01) << (cpuIndex % 8);
	return map;
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

	while(!is_exit)
	// Run the CpuScheduler function that checks the CPU Usage and sets the pin at an interval of "interval" seconds
	{
		CPUScheduler(conn, interval);
		sleep(interval);
	}

	// Closing the connection
	virConnectClose(conn);
	return 0;
}

/* COMPLETE THE IMPLEMENTATION */
void CPUScheduler(virConnectPtr conn, int interval)
{
	// Get the total number of pCpus in the host
	unsigned int onlineCPUs = 0;
	unsigned char * cpuMap;
	int ans = virNodeGetCPUMap(conn, &cpuMap, &onlineCPUs, 0);
	if(ans == -1) {
		fprintf(stderr, "Error in virNodeGetCPUMap call");
		exit(0);
	}
	total_pcpus = ans;
	free(cpuMap);

	printf("# pCPUs is %d\n", total_pcpus);

	// Using the number of pCPUs, allocate memory and initialize the pcpuUsage array.
	pcpuUsage = malloc(sizeof(unsigned long long int) * total_pcpus);
	memset(pcpuUsage, 0, sizeof(unsigned long long int) * total_pcpus);

	// Get the total number of vCpus in the host
	int numDomains = virConnectNumOfDomains(conn);
	if(numDomains == -1) {
		fprintf(stderr, "Error in virConnectNumOfDomains call");
		exit(0);
	}
	total_vcpus = numDomains;

	// Using the number of vCPUs, allocate memory and initialize the vcpuUsage array.
	if(firstIteration == 1) {
		vcpuUsage = malloc(sizeof(unsigned long long int) * total_vcpus);
		memset(vcpuUsage, 0, sizeof(unsigned long long int) * total_vcpus);
		vcpuarray = malloc(sizeof(vcpuNode) * total_vcpus);
		firstIteration = 0;
	}

	virDomainPtr *domainsList;
	int total_domains;

	unsigned int flags = VIR_CONNECT_LIST_DOMAINS_RUNNING |
						VIR_CONNECT_LIST_DOMAINS_PERSISTENT;
	total_domains = virConnectListAllDomains(conn, &domainsList, flags);
	if (total_domains < 0) {
		printf("Error in call to virConnectListAllDomains\n");
		exit(0);
	}
	printf("# Active domains is %d\n", total_domains);

	virVcpuInfoPtr info;
	info = malloc(sizeof(virVcpuInfoPtr));
	for(int i = 0; i < total_domains; i++) {
		int ret = virDomainGetVcpus(domainsList[i], info, 1, NULL, 0);
		if(ret == -1) {
			fprintf(stderr, "Error in call to virDomainGetVcpus");
			exit(0);
		}
		unsigned long long int diff = info->cpuTime - vcpuUsage[i];
		// printf("Usage for vCPU %d for the interval is %llu, CPU is %d\n", i, diff, info->cpu);
		vcpuUsage[i] = info->cpuTime;
		pcpuUsage[info->cpu] +=  diff;
		vcpuarray[i].usage = diff / DIV_FACTOR;
		vcpuarray[i].id = i;
	}

	double* pcpuUsageNormalized = malloc(sizeof(double) * total_pcpus);
	for(int i = 0; i < total_pcpus; i++) {
		pcpuUsageNormalized[i] = (double)(pcpuUsage[i] / DIV_FACTOR);
		pcpuUsage[i] = 0; // Need to set it to zero for next run
	}

	double sum = 0.0, mean, SD = 0.0;
	int i;
	for (i = 0; i < total_pcpus; ++i) {
		sum += pcpuUsageNormalized[i];
	}
	mean = sum / total_pcpus;
	for (i = 0; i < total_pcpus; ++i) {
		SD += pow(pcpuUsageNormalized[i] - mean, 2);
	}
	SD = sqrt(SD / total_pcpus);
	double percent = 100 * SD / mean;
	printf("SD is %f, percent deviation is %f\n", SD, percent);

	if(percent <= 5.0) {
		printf("Less than 5 percent, no need to change order, returning!\n");
		return;
	} else {
		printf("Greater than 5 percent, deteced imbalance, changing!\n");
		memset(pcpuUsageNormalized, 0, sizeof(double) * total_pcpus);

		// Sort vCPU usage in descending order
		for(int i = 0; i < total_domains-1; i++) {
			for(int j = 0; j < total_domains - i - 1; j++) {
				if(vcpuarray[j].usage < vcpuarray[j+1].usage) {
					double tmp = vcpuarray[j].usage;
					vcpuarray[j].usage = vcpuarray[j+1].usage;
					vcpuarray[j+1].usage = tmp;
					int tmp2 = vcpuarray[j].id;
					vcpuarray[j].id = vcpuarray[j+1].id;
					vcpuarray[j+1].id = tmp2;
				}
			}
		}

		for(int i = 0; i < total_domains; i++) {
			double minPcpuusage = pcpuUsageNormalized[0];
			int minPcpuusageindex = 0;
			for(int j = 1; j < total_pcpus; j++) {
				if(minPcpuusage > pcpuUsageNormalized[j]) {
					minPcpuusage = pcpuUsageNormalized[j];
					minPcpuusageindex = j;
				}
			}
			// printf("Min usage %f and min usage CPU is %d\n", minPcpuusage, minPcpuusageindex);
			pcpuUsageNormalized[minPcpuusageindex] += vcpuarray[i].usage;

			unsigned char* map = generateMap(total_pcpus, minPcpuusageindex);

			// printf("map is %d \n", *map);
			int status = virDomainPinVcpu(domainsList[i], 0, map, VIR_CPU_MAPLEN(total_pcpus));
			if(status == -1) {
				printf("Failure in assigning!\n");
			} else {
				printf("Pinned VM %d to PCPU %d\n", i, minPcpuusageindex);
			}
		}
	}
	free(info);
}

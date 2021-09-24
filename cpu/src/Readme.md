## Project Instruction Updates:

1. Complete the function CPUScheduler() in vcpu_scheduler.c
2. If you are adding extra files, make sure to modify Makefile accordingly.
3. Compile the code using the command `make all`
4. You can run the code by `./vcpu_scheduler <interval>`
5. While submitting, write your algorithm and logic in this Readme.

## Algorithm Description
I am using the Greedy Number partitioning algorithm for partitioning the vCPUs into the pCPUs.
For the purposes of our discussion, one domain is associated with exactly one vCPU, so I will use domain and vCPU interchangably.

My approach is as follows:
I compute the cpu time consumed by a domain in the past interval. This is domain for every domain by keeping a track of the current
cpu time, and the cpu time at the previous instance the scheduler ran. Their difference gives me the utilization for the past interval.

I also compute the cpu time by a particular pCPU by utilizing the mapping between vCPU and pCPU as provided in the virVcpuInfoPtr struct.

Now, I calculate the standard deviation of the pCPU usages. If the standard deviation is less than 5% of the mean, there is no need to
schedule anything. The CPU Scheduler just exits.

However, if the standard deviation is greater than 5%, then I will use the greedy number partitioning algorithm.
The algorithm is as follows:

1. Sort the vCPU usages in descending order.
2. Initialize pCPU buckets to zero for each pCPU.
3. Traverse through the list of vCPU usages:
    a. For a vCPU, select the pCPU which is the least utilized rn. (Initially all are at 0)
    b. Pin that vCPU to that pCPU and increase the usage of the pCPU by the vCPU usage.

The link to the algorithm is as follows: https://en.wikipedia.org/wiki/Greedy_number_partitioning "Ordered Algorithm" section.
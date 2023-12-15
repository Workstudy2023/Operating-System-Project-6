// Author: Christine Mckelvey
// Date: December 14, 2023

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <getopt.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>

// GLOBAL VARIABLES

#define SHM_KEY 205431  
#define PERMS 0644    

// store the total number of entries allowed in our frame table
#define FRAME_TABLE_SIZE 256  

// simulated clock
unsigned int simClock[2] = {0, 0};

// shared memory variables
unsigned shmID;             
unsigned* shmPtr; 

// Message queue structure
typedef struct messages {
    long mtype; // allows the parent to know its receiving a message    
    int address; // address child wants to read or write
    int option;  // option is set to 1 if worker wants to read, 0 if worker wants to write]
    int willTerminate; // the parent needs to know if the child will terminate (similar to project 4 scheduling)
} messages;

int msgqId; // message queue ID
messages buffer; // message queue Buffer


// stores the time when a request gets to the front of the queue
unsigned int timeAtFrontOfQueue[2] = {0, 0}; 

// Process Control Block structure
struct PCB {
    pid_t pid;   // process id of this child    
    int occupied; // either true or false
    int startSeconds; // time when it was created
    int startNano; // time when it was created
    int pages[32];  // the page table for this child process
   
    int totalMemoryReferences;  // memory references made by this child
    int requestedPageIndex;  // page table index that the process wants to their request in
    int inQueue; // process is blocked and is in our queue
    int atQueueFront; // does this process have a request at the front of the queue
};
struct PCB childProcessTable[18];

// Frame Table structure
struct FrameTable {
    int dirtyBit; // frames dirty bit (has the frame been written to)
    int pageOccupied; // which page is in this frame 
    pid_t processOfPage; // the Id of the process whose page is in this frame
    int HeadOfFifo; // flag set if the frame is head of FIFO queue
};
struct FrameTable frameTable [FRAME_TABLE_SIZE];

// FIFO queue array used for page replacement,
int fifoQueue[18];

// process timing variables for events
unsigned int halfSecond = 500000000; // half a second in nanoseconds
unsigned int halfSecondPassed = 0; // flag if half a second passed
unsigned int fulfillTime = 14000000; // 14 milliseconds, time we need to wait for the request at the head of the queue to be fulfilled

// process command line launching variables
char* filename = NULL; // logfile.txt
int processCount;      
int simultaneousCount; 
int processSpawnRate;  

// process statistics variables
int totalNumberOfReferencesMade = 0; // statistic for number of memory references made
int totalNumberOfPageFaults = 0; // statistic for number of page faults

// process launching variables
int totalLaunched = 0;
int totalTerminated = 0;
unsigned long long launchTimePassed = 0;

// Function prototypes
void launchChildren(); // function to launch new processes
void childMessages(int childIndex); // function to send and check child messages
void replacementAlgorithm(); // function to swap items from the queue with the oldest frame in the frame table

void addToQueue(); // function to add an item to the queue
void removeFromQueue(int index); // function to remove an item from the queue
void runFIFOQueueCheck(); // sees if the FIFO queue is full and trys to grant a request

void showProcessTable(); // function to print process table
void showMemoryTables();  // function to print the frame and page table
void pageNotInMemory(int r, int p, int a, int poa); // function to handle a page not being in memory
void pageIsInMemory(int r, int p, int a, int poa); // function to handle a page being in memory

void incrementSimulatedClock(int nanoseconds); // function to increase clock time
void handleTermination(); // function to terminate program and clean up system

// Function to setup argument handling and initialize structures
int main(int argc, char** argv) {
    // register signal handlers for interruption and timeout
    srand(time(NULL) + getpid());
    signal(SIGINT, handleTermination);
    signal(SIGALRM, handleTermination);
    alarm(5); 

    // check arguments
    char argument;
    while ((argument = getopt(argc, argv, "f:hn:s:t:")) != -1) {
        switch (argument) {
            case 'f': {
                char* opened_file = optarg;
                FILE* file = fopen(opened_file, "r");
                if (file) 
                {
                    filename = opened_file;
                    fclose(file);
                } 
                else {
                    printf("file doesn't exist.\n");
                    exit(1);
                }
                break;
            }           
            case 'h':
                printf("\noss [-h] [-n proc] [-s simul] [-t timeToLaunchNewChild] [-f logfile]\n");
                printf("h is the help screen\n"
                    "n is the total number of child processes oss will ever launch\n"
                    "s specifies the maximum number of concurrent running processes\n"
                    "t is for processes speed as they will trickle into the system at a speed dependent on parameter\n"
                    "f is for a logfile as previously\n\n");
                exit(0);
            case 'n':
                processCount = atoi(optarg);
                if (processCount > 18) {
                    printf("invalid processCount\n");
                    exit(1);
                }                
                break;
            case 's':
                simultaneousCount = atoi(optarg);
                if (simultaneousCount > 18) {
                    printf("invalid simultaneousCount\n");
                    exit(1);
                }
                break;
            case 't':
                processSpawnRate = atoi(optarg);
                break;
            default:
                printf("invalid commands\n");
                exit(1);
        }
    }

    // verify arguments
    if (processCount == 0 || processSpawnRate == 0 || simultaneousCount == 0 || filename == NULL) 
    {
        printf("invalid commands\n");
        exit(1);
    }   

    // create message queue file
    system("touch msgq.txt"); 

    // initialize the process table
    for (int i = 0; i < 18; i++)  
    {

        childProcessTable[i].pid = 0;
        childProcessTable[i].occupied = 0;
        childProcessTable[i].inQueue = 0;
        childProcessTable[i].atQueueFront = 0;
        childProcessTable[i].startNano = 0;
        childProcessTable[i].startSeconds = 0;
        childProcessTable[i].totalMemoryReferences = 0;
        childProcessTable[i].requestedPageIndex = INT16_MIN;
        
        // initialize the processes page table
        for (int k=0; k<32; k++) 
        {
            // set it to be a negative number so we know its empty
            childProcessTable[i].pages[k] = INT16_MIN;
        }
    }

    // initialize the frame table
    for (int i = 0; i < FRAME_TABLE_SIZE; i++)
    {
        frameTable[i].pageOccupied = -1;
        frameTable[i].processOfPage = -1;
        frameTable[i].dirtyBit = 0;
        frameTable[i].HeadOfFifo = -1;
    }

    // initialize the fifoQueue with negative values so we know 
    // that there isnt a page in that spot 
    for (int j=0; j<18; j++) 
    {
        fifoQueue[j] = INT16_MIN;
    }

    // make shared memory
    shmID = shmget(SHM_KEY, sizeof(unsigned) * 2, 0777 | IPC_CREAT);
    if (shmID == -1) 
    {
        perror("Unable to acquire the shared memory segment.\n");
        handleTermination();
    }
    
    shmPtr = (unsigned*)shmat(shmID, NULL, 0);
    if (shmPtr == NULL) 
    {
        perror("Unable to connect to the shared memory segment.\n");
        handleTermination();
    }
    memcpy(shmPtr, simClock, sizeof(unsigned) * 2);

    // make message queue
    key_t messageQueueKey = ftok("msgq.txt", 1);
    if (messageQueueKey == -1) 
    {
        perror("Unable to generate a key for the message queue.\n");
        handleTermination();
    }

    msgqId = msgget(messageQueueKey, PERMS | IPC_CREAT);
    if (msgqId == -1) 
    {
        perror("Unable to create or access the message queue.\n");
        handleTermination();
    }

    // start main program code
    launchChildren();
    return 0;
}

// Function to launch new children
void launchChildren() {
    // run code until all the processes have ended
    while (totalTerminated != processCount) {
        // update our simulated clock
        launchTimePassed += 100000; 
        incrementSimulatedClock(launchTimePassed);
    
        // determine if we should launch a child
        if (launchTimePassed >= processSpawnRate || totalLaunched == 0) 
        {
            if (totalLaunched < processCount && totalLaunched < simultaneousCount + totalTerminated) {
                // launch new child
                pid_t pid = fork();
                if (pid == 0) 
                {
                    char* args[] = {"./worker", NULL};
                    execvp(args[0], args);
                }
                else 
                {
                    childProcessTable[totalLaunched].pid = pid;
                    childProcessTable[totalLaunched].occupied = 1;
                    childProcessTable[totalLaunched].startSeconds = simClock[0];
                    childProcessTable[totalLaunched].startNano = simClock[1];
                } 
                totalLaunched += 1;
            }

            launchTimePassed = 0;
        }

        // check if we should stop the program
        if (processCount == totalTerminated) {
            handleTermination();
        }

        // check and send messages to the child processes
        for (int k=0; k<totalLaunched; k++)
        {
            // only send a message to processes that aren't
            // in our fifoQueue because those are blocked currently
            if (childProcessTable[k].occupied == 1 && childProcessTable[k].inQueue == 0) {
                // send and check message with memory address request from this child
                childMessages(k);
            }
        }

        // show all the process table, frame table, and page tabler after half a second has passed
        if (simClock[1] >= halfSecondPassed + halfSecond || (simClock[1] == 0 && simClock[0] > 1)) 
        {
            showProcessTable();
            showMemoryTables();
            halfSecondPassed = simClock[1];
        }
    }

    // all processes terminated so we clean up our code and output 
    // the ending program statistics
    handleTermination();
}

// Function to send and check child messages
void childMessages(int childIndex) {
    // send a message to the child process
    buffer.mtype = childProcessTable[childIndex].pid;
    if (msgsnd(msgqId, &buffer, sizeof(messages)-sizeof(long), IPC_NOWAIT) == -1) 
    {
        fprintf(stderr, "Error, msgsnd to worker failed\n");
        exit(EXIT_FAILURE);
    }

    // after we sent a message to the child
    // lets check what address they want and if its a read or write 
    messages childMsg;
    if (msgrcv(msgqId, &childMsg, sizeof(messages), getpid(), 0) == -1) 
    {
        fprintf(stderr, "Error, failed to receive message in parent\n");
        exit(EXIT_FAILURE);
    }	

    // open up our log file so we can print information to it
    // about the request we got back from the child process
    FILE* file = fopen(filename, "a+");
    if (file == NULL) {
        perror("Error opening file");
        handleTermination();
    }

    // first check if the child wants to terminate
    if (childMsg.willTerminate == 1) {
        // child wants to terminate so we have to clear its resources
        // and print the child ending messages
        fprintf(file, "\noss: detected process P%d terminated at time: %u, %u\n", childIndex, simClock[0], simClock[1]);
        printf("\noss: detected process P%d terminated at time: %u, %u\n", childIndex, simClock[0], simClock[1]);
        
        // child has terminated so we output the child's final memory reference statistics
        fprintf(file, "oss: Process P%d had %d total memory references before terminating\n", childIndex, childProcessTable[childIndex].totalMemoryReferences);
        printf("oss: Process P%d had %d total memory references before terminating\n", childIndex, childProcessTable[childIndex].totalMemoryReferences);

        // update the process table and total termination count
        childProcessTable[childIndex].occupied = 0;
        childProcessTable[childIndex].inQueue = 0;
        totalTerminated += 1;
        fclose(file);
        return; // Leave function because this child has terminated
    }

    fclose(file);

    // Get the requested memory address and if the child wants a read or write of the memory address
    int readOrWriteDecision = childMsg.option;
    int childRequestedAddress = childMsg.address;

    // Calculate the page that the childs requested address would be in their page table
    int pageTableIndex = (int)childRequestedAddress/1024;

    // Now we have all the information from the child process
    // determine if the request can be fulfilled. That is, if the page is in memory
    // and if we can store it in our frame table or if the frame table is empty

    // HANDLE CHILD WANTING WRITE OR READ OF MEMORY ADDRESS



    if (childProcessTable[childIndex].pages[pageTableIndex] >= 0)
    {
        // The requested child page is already loaded into memory so call pageIsInMemory function
        pageIsInMemory(readOrWriteDecision, childIndex, childRequestedAddress, pageTableIndex);
    }
    else 
    {
        // The requested child page is not loaded into memory yet so call pageNotInMemory function
        pageNotInMemory(readOrWriteDecision, childIndex, childRequestedAddress, pageTableIndex);
    }

    // see if the queue is full and if it is
    // try to grant the frame request for the first process at the beginning of our fifoQueue
    runFIFOQueueCheck();
}

// Function to handle when the page is in memory
void pageIsInMemory(int readOrWrite, int processIndex, int addressChildRequested, int pageOfAddress) {
    // Open the file in append mode
    FILE* file = fopen(filename, "a+");
    if (file == NULL) {
        perror("Error opening file");
        handleTermination();
    }

    // print read or write messages
    if (readOrWrite == 0) {
        // print child process wanting read of address
        fprintf(file, "\noss: P%d requesting read of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
        printf("\noss: P%d requesting read of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);

        // print oss giving read of address message
        fprintf(file, "\noss: Indicating to process %d that read has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
        printf("\noss: Indicating to process %d that read has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);;
    }
    else {
        // print child process wanting write of address
        fprintf(file, "\noss: P%d requesting write of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
        printf("\noss: P%d requesting write of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);

        // print oss giving write of address message
        fprintf(file, "\noss: Indicating to process %d that write has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
        printf("\noss: Indicating to process %d that write has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);

        // update dirty bit and print message
        // if the dirty bit is set to 0, meaning the frame hasnt been written to yet
        int frameIndexOfPage = childProcessTable[processIndex].pages[pageOfAddress];
        if (frameTable[frameIndexOfPage].dirtyBit == 0) 
        {
            // increment clock additionally because we are changing the dirty bit
            frameTable[frameIndexOfPage].dirtyBit = 1;
            incrementSimulatedClock(100000);

            // print dirty bit message to file and screen
            fprintf(file, "oss: Dirty bit of frame %d, set for the first time at time: %u:%u\n", frameIndexOfPage, simClock[0], simClock[1]);
            printf("oss: Dirty bit of frame %d, set for the first time at time: %u:%u\n", frameIndexOfPage, simClock[0], simClock[1]);
        }
    }

    // update clock normally because a memory reference was made
    incrementSimulatedClock(100000);

    // update the childs total memory references made and the total references made in the program
    childProcessTable[processIndex].totalMemoryReferences += 1;
    totalNumberOfReferencesMade += 1;
    fclose(file);
}   

// Function to handle a page not being in memory
void pageNotInMemory(int readOrWrite, int processIndex, int addressChildRequested, int pageOfAddress) {
    // Open the file in append mode
    FILE* file = fopen(filename, "a+");
    if (file == NULL) {
        perror("Error opening file");
        handleTermination();
    }

    if (readOrWrite == 0)
    {
        // child wants to read from memory but page not loaded in, print message
        fprintf(file, "\noss: P%d requesting read of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
        printf("\noss: P%d requesting read of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
    }   
    else 
    {
        // print child process wanting write of address
        fprintf(file, "\noss: P%d requesting write of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
        printf("\noss: P%d requesting write of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
    }

    // find a frame to store the page that's not in memory.
    // we will find a page if  if there are any empty frames
    for (int k=0; k<256; k++)
    {
        struct FrameTable currentFrame = frameTable[k];
        if (currentFrame.pageOccupied == -1)
        {
            // print child process wanting write of address
            fprintf(file, "\noss: P%d requesting write of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
            printf("\noss: P%d requesting write of address %d at time %u:%u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);

            // print page fault message first
            fprintf(file, "\noss: Address %d is not a frame, system pagefault.\n", addressChildRequested);
            printf("\noss: Address %d is not a frame, system pagefault.\n", addressChildRequested);

            // set dirty bit to 1 since we are writing to this frame for the first time
            currentFrame.dirtyBit = 1;
            incrementSimulatedClock(100000);

            // print dirty bit message
            fprintf(file, "\noss: Dirty bit of frame %d was set, now adding additional time to clock\n", k);
            printf("\noss: Dirty bit of frame %d was set, now adding additional time to clock\n", k);

            // store the page that will occupy this frame
            currentFrame.pageOccupied = pageOfAddress;
            // store the process ID for this page so we can know whose page the frame has
            currentFrame.processOfPage = childProcessTable[processIndex].pid;

            // update the page table of the child whose page is now going to be added to the frame table
            childProcessTable[processIndex].pages[pageOfAddress] = k; // where k represents the frame index this page will be stored in

            // update the total memory references made from by child 
            childProcessTable[processIndex].totalMemoryReferences += 1;

            // print frame table update success message
            // now the processes page is stored in the frame table
            // indicating to p2 that write has happened to address x
            // or read
            if (readOrWrite == 0)
            {
                // print read success message
                fprintf(file, "\noss: Indicating to process %d that read has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
                printf("\noss: Indicating to process %d that read has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
            }
            else 
            {
                // print write success message
                fprintf(file, "\noss: Indicating to process %d that write has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
                printf("\noss: Indicating to process %d that write has happened to address %d at time: %u, %u\n", processIndex, addressChildRequested, simClock[0], simClock[1]);
            }

            // and update the system total statistics
            totalNumberOfReferencesMade += 1;
            totalNumberOfPageFaults += 1; 
            return;
        }
    }

    // update memory usage statistics
    totalNumberOfReferencesMade += 1;
    totalNumberOfPageFaults += 1; // since the page was not in memory, we need to update page faults

    // increment the clock and close the file
    incrementSimulatedClock(100000);
    fclose(file);
}










// Function to see if the FIFO queue is full and trys to grant a request to the first process
void runFIFOQueueCheck() {
    // iterate the queue and get the first item
    if (fifoQueue[0] != -1)
    {
        // theres an item at the front of the queue
        // todo
    }
}

// Function to show the frame table and page table
void showMemoryTables() {
    // print information to screen and log file
    FILE* file = fopen(filename, "a+");
    if (file == NULL) {
        perror("Error opening file");
        handleTermination();
    }    

    // print page table information
    // print each childs page table entry
    fprintf(file, "\n");
    printf("\n");
    fprintf(file, "%-8s%-8s", "", "\t\t\t\t\t\tPage Table Entries: \n");
    printf("%-8s%-8s", "", "\t\t\t\t\t\tPage Table Entries: \n");
    
    for (int i = 0; i < 32; ++i) {
        if (i == 0) {
            printf("P %-6s", " ");
            fprintf(file, "%-6s", " ");
        }
        printf("%-4d", i);
        fprintf(file, "%-4d", i);
    }
    fprintf(file, "\n");
    printf("\n");
    fprintf(file, "\n");
    printf("\n");  

    for (int i = 0; i < totalLaunched; i++)
    {
        if (childProcessTable[i].occupied == 1) 
        {
            // print pages for this child process
            printf("%-8d", i);
            fprintf(file, "%-8d", i);
            for (int j = 0; j < 32; ++j) {
                // print an empty space if the number is negative
                // meaning the page isn't in the frame table
                if (childProcessTable[i].pages[j] < 0)
                {
                    // value not in frame table
                    printf("%-4s", "*");
                    fprintf(file, "%-4s", "*");
                }
                else
                {
                    // value in frame table
                    printf("%-4d", childProcessTable[i].pages[j]);
                    fprintf(file, "%-4d", childProcessTable[i].pages[j]);
                }

            }
            printf("\n");
            fprintf(file, "\n");
        }
    }
    printf("\n");  
    fprintf(file, "\n");

    // print frame table information
    // prints which processes have a page in the frame table
    fprintf(file, "\n");
    printf("\n");
    fprintf(file, "%-13s%-15s%-15s%-15s\n", "Frame Index", "Occupied", "Dirty Bit", "HeadOfFifo");
    printf("%-13s%-15s%-15s%-15s\n", "Frame Index", "Occupied", "Dirty Bit", "HeadOfFifo");
    
    for (int i = 0; i < FRAME_TABLE_SIZE; ++i) 
    {
        struct FrameTable currentFrame = frameTable[i];
        if (currentFrame.HeadOfFifo == 1) {
            fprintf(file, "%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "*");
            printf("%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "*");
        }
        else {
            fprintf(file, "%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "");
            printf("%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "");
        }
    }
    printf("\n");  
    fprintf(file, "\n");

    // Close the file
    fclose(file);
}

// Function to print the process table
void showProcessTable() {
    // Open the file in append mode
    FILE* file = fopen(filename, "a+");

    if (file == NULL) {
        perror("Error opening file");
        handleTermination();
    }

    // Print to the file
    fprintf(file, "\nOSS PID: %d SysClockS: %d SysclockNano: %d\nProcess Table: \n%-6s%-10s%-8s%-12s%-12s\n",
            getpid(), simClock[0], simClock[1], "Entry", "Occupied", "PID", "StartS", "StartN");

    for (int i = 0; i < totalLaunched; i++) {
        fprintf(file, "%-6d%-10d%-8d%-12u%-12u\n",
                i, childProcessTable[i].occupied, childProcessTable[i].pid, childProcessTable[i].startSeconds, childProcessTable[i].startNano);
    }
    fprintf(file, "\n");

    // Print to the screen
    printf("\nOSS PID: %d SysClockS: %d SysclockNano: %d\nProcess Table: \n%-6s%-10s%-8s%-12s%-12s\n",
            getpid(), simClock[0], simClock[1], "Entry", "Occupied", "PID", "StartS", "StartN");

    for (int i = 0; i < totalLaunched; i++) {
        printf("%-6d%-10d%-8d%-12u%-12u\n",
                i, childProcessTable[i].occupied, childProcessTable[i].pid, childProcessTable[i].startSeconds, childProcessTable[i].startNano);
    }
    printf("\n");

    // Close the file
    fclose(file);
}

// Function to update the clock by given amount of nanoseconds
void incrementSimulatedClock(int nanoseconds) {
    // increment the clock and then determine if we need to adjust
    // the seconds
    simClock[1] += nanoseconds;
    if (simClock[1] >= 1000000000) 
    {
        // Calculate the number of seconds to add
        // Update seconds and adjust nanoseconds
        unsigned secondsToAdd = simClock[1] / 1000000000;
        simClock[0] += secondsToAdd;
        simClock[1] %= 1000000000;
    }

    memcpy(shmPtr, simClock, sizeof(unsigned int) * 2);
}

// Function to clean up the memory and output memory statistics
void handleTermination() {
    // output the final data
    float memoryStatistic = 0;
    float pageFaultsStatistic= 0;

    FILE* file = fopen(filename, "a+");
    if (file == NULL) {
        perror("Error opening file");
        handleTermination();
    }

    // determine output statistic for memory references made
    // calculation given by: totalNumberOfReferencesMade/seconds
    if (totalNumberOfReferencesMade != 0 && simClock[0] != 0) {
        memoryStatistic = (float)totalNumberOfReferencesMade / simClock[0];
    }

    // determine output statistic for page faults
    // calculation given by: totalNumberOfPageFaults/totalNumberOfReferencesMade
    if (totalNumberOfPageFaults != 0 && totalNumberOfReferencesMade != 0) {
        pageFaultsStatistic = (float)(totalNumberOfPageFaults) / (totalNumberOfReferencesMade);
    }

    // print statistic
    if (memoryStatistic == 0) {
        // print total memory accesses instead
        printf("\nProgram finished, the memory accesses in the system is: %d\n", totalNumberOfReferencesMade);
        fprintf(file, "\nProgram finished, the memory accesses in the system is: %d\n", totalNumberOfReferencesMade);
    } 
    else {
        // we will output the memory accesses per second in the system
        printf("\nProgram finished, the memory accesses per second in the system is: %.1f\n", memoryStatistic);
        fprintf(file, "\nProgram finished, the memory accesses per second in the system is: %.1f\n", memoryStatistic);
    }

    if (pageFaultsStatistic == 0) {
        // print total page faults instead
        printf("Program finished, the number of page faults in the system was: %d\n", totalNumberOfPageFaults);
        fprintf(file, "Program finished, the number of page faults in the system was: %d\n", totalNumberOfPageFaults);
    }
    else {
        // we will output the number of page faults per memory access in the system
        printf("Program finished, the number of page faults per memory access in the system is: %.1f\n", pageFaultsStatistic);
        fprintf(file, "Program finished, the number of page faults per memory access in the system is: %.1f\n", pageFaultsStatistic);
    }

    // close the file
    fclose(file);

    // clean msg queue and shared memory
    msgctl(msgqId, IPC_RMID, NULL);
    shmdt(shmPtr);
    shmctl(shmID, IPC_RMID, NULL);

    // kill all child processes
    kill(0, SIGTERM);
}






// // Function prototypes
// void replacementAlgorithm(); // function to swap items from the queue with the oldest frame in the frame table
// void addToQueue(); // function to add an item to the queue
// void removeFromQueue(int index); // function to remove an item from the queue
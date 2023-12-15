// Author: Christine Mckelvey
// Date: December 14, 2023

#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/msg.h>
#include <string.h>

// Globals
#define SHM_KEY 205431
#define PERMS 0644

unsigned int simClock[2]; // gives child current simulated clock time

// Message queue structure
typedef struct messages {
    long mtype; // allows the parent to know its receiving a message    
    int address; // address child wants to read or write
    int option;  // option is set to 0 if worker wants to read, 1 if worker wants to write
    int willTerminate; // the parent needs to know if the child will terminate (similar to project 4 scheduling)
} messages;

int queueID;  
messages msgBuffer;   

// how many memory references this child has made
int totalChildMemoryReferences = 0; 

// Function prototypes
void shouldChildTerminate();
void executeChildTask();

// Function to start
int main(int argc, char const *argv[]) {
    // generate randomness
    srand(time(NULL) + getpid());

    // set up the message queue
    key_t msgQueueKey = ftok("msgq.txt", 1);
    if (msgQueueKey == -1) {
        perror("Child failed to generate a in key using ftok.\n");
        exit(1);
    }
    
    queueID = msgget(msgQueueKey, PERMS);
    if (queueID == -1) {
        perror("Child failed to access the message queue.\n");
        exit(1);
    }

    // call the childTask to start
    executeChildTask();
    return 0;
}

// Function to update clock, check timer and get initial parent messages
void executeChildTask() { 
    // receive and send messages
    while (1) 
    {
        // get a message from the parent process
        if (msgrcv(queueID, &msgBuffer, sizeof(msgBuffer), getpid(), 0) == -1) 
        {
            perror("Failed to receive a message in the child.\n");
            exit(1);
        }

        // get the current clock time from shared memory
        int sharedMemID = shmget(SHM_KEY, sizeof(int) * 2, 0777);
        if (sharedMemID == -1) 
        {
            perror("Error: Failed to access shared memory using shmget.\n");
            exit(EXIT_FAILURE);
        }

        int* sharedMemPtr = (int*)shmat(sharedMemID, NULL, SHM_RDONLY);
        if (sharedMemPtr == NULL) 
        {
            perror("Error: Failed to attach to shared memory using shmat.\n");
            exit(EXIT_FAILURE);
        }

        // give the child the new clock time
        simClock[0] = sharedMemPtr[0]; // seconds
        simClock[1] = sharedMemPtr[1]; // nanoseconds
        shmdt(sharedMemPtr);

        // set the default termination behavior to be to not terminate
        msgBuffer.willTerminate = 0;
        // check to see if the child wants to terminate
        // if msgBuffer.willTerminate becomes 1 after we call shouldChildTerminate, that means the child will terminate
        shouldChildTerminate();
    
        // calculate a random memory address from memory 0 to memory 32000 to send to parent
        int memoryAddress = rand() % 32001;

        // determine what the child wants to do: read or write to the address 
        int determineReadOrWrite = rand() % 101;
        if (determineReadOrWrite <= 25) {
            // number is less or equal to 25 (25 percent chance of writing) so child will send a write message to parent
            msgBuffer.option = 1;
        }   
        else {
            // number is greater than 25 (75 percent chance of reading) so child will send a read message to parent
            msgBuffer.option = 0;
        }

        // increment the memory references (messages sent to the parent) this child has made
        totalChildMemoryReferences += 1;
        
        // send message containing the memory address and if we want to read or write
        msgBuffer.mtype = getppid();
        msgBuffer.address = memoryAddress;
    
        if (msgsnd(queueID, &msgBuffer, sizeof(messages) - sizeof(long), 0) == -1) {
            perror("msgsnd to parent failed\n");
            exit(1); // FAILURE
        }

        // terminate the child after sending the message if they wanted to terminate
        if (msgBuffer.willTerminate == 1) {
            exit(0); // SUCCESS
        }

    }

}

// Function to check for child termination after every 1000 memory references made
void shouldChildTerminate() {
    // we will try to terminate the child after 
    // a total number of 1000 memory references from this child has been made
    // so we run the code every 1000 memory references (e.g at 1000, 2000, etc)
    if (totalChildMemoryReferences % 1000 == 0) 
    {
        if (totalChildMemoryReferences != 0)
        {
            // child has a 25 % chance of ending
            int randTerm = rand() % 101;
            if (randTerm <= 25) 
            {
                // store terminate message to send to parent
                msgBuffer.willTerminate = 1;
            }
        }
    }
}

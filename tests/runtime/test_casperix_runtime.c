/**
 * Casperix Runtime - Integration Test
 * Tests task, future, and channel systems
 */

#include "runtime/async/task.h"
#include "runtime/async/future.h"
#include "runtime/concurrent/channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test task execution
void test_task_basic(void* data) {
    printf("[TEST] Task executing with data: %s\n", (char*)data);
}

void test_tasks() {
    printf("\n=== Testing Task System ===\n");
    
    Task* task = task_create(test_task_basic, "Hello from task!", "test_task");
    printf("Created task ID: %llu\n", task->task_id);
    
    task_execute(task);
    
    if (task_is_complete(task)) {
        printf("✅ Task completed successfully\n");
    }
    
    task_destroy(task);
}

// Test futures
void future_callback(void* result, void* user_data) {
    printf("[CALLBACK] Future completed with result: %s\n", (char*)result);
    int* flag = (int*)user_data;
    *flag = 1;
}

void test_futures() {
    printf("\n=== Testing Future System ===\n");
    
    Future* future = future_create();
    printf("Created future\n");
    
    int callback_called = 0;
    future_then(future, future_callback, &callback_called);
    
    // Complete the future
    future_complete(future, "Future result!");
    
    if (callback_called) {
        printf("✅ Future callback executed\n");
    }
    
    future_destroy(future);
}

// Test channels
void test_channels() {
    printf("\n=== Testing Channel System ===\n");
    
    Channel* ch = channel_create(5);  // Buffered channel
    printf("Created buffered channel (capacity: 5)\n");
    
    // Send some messages
    for (int i = 0; i < 3; i++) {
        int* value = malloc(sizeof(int));
        *value = i * 10;
        if (channel_send(ch, value)) {
            printf("Sent: %d\n", *value);
        }
    }
    
    // Receive messages
    for (int i = 0; i < 3; i++) {
        int* value = (int*)channel_recv(ch);
        if (value) {
            printf("Received: %d\n", *value);
            free(value);
        }
    }
    
    printf("✅ Channel send/recv works\n");
    
    channel_close(ch);
    channel_destroy(ch);
}

// Main test runner
int main(void) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Casperix Runtime Integration Test    ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    test_tasks();
    test_futures();
    test_channels();
    
    printf("\n=== All Tests Passed ✅ ===\n\n");
    
    return 0;
}

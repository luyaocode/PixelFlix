#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdarg.h>
#include "logger.h"

void logger(int type , const char* format , ...)
{
    pthread_t current_thread = pthread_self();
    va_list args;
    va_start(args , format);
    switch (type)
    {
    case EXIT_FAILURE:
    {
        char buffer[1000];
        vsnprintf(buffer , sizeof(buffer) , format , args);
        fprintf(stderr , "[ 0x%lx ] exit(%d): %s\n" , current_thread , EXIT_FAILURE , buffer);
        exit(EXIT_FAILURE);
    }
    case 2:
    {
        char buffer[1000];
        vsnprintf(buffer , sizeof(buffer) , format , args);
        printf("[ 0x%lx ] : %s\n" , current_thread , buffer);
    }
    default:break;
    }
    va_end(args);
}
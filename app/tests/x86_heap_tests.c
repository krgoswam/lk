/*
 * Copyright (c) 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <app/tests.h>
#include <stdio.h>
#include <arch/mmu.h>
#include <err.h>
#include <debug.h>
#include <arch/x86/mmu.h>
#include <err.h>
#include <malloc.h>
#include <lib/heap.h>


int x86_heap_tests(void)
{
	int *testbuffer;
	char *testchar = NULL;
	printf("---- x86 HEAP Test: Launching malloc tests ----\n");
	printf("Allocating an integer testbuffer for size int\n");
	testbuffer = (int *)malloc(sizeof(int));
	printf("Writing to memory address 0x%llx returned by malloc\n",testbuffer);
	*testbuffer = 5600;
	printf("address of testbuffer = 0x%llx , value of testbuffer = %d\n",testbuffer,*testbuffer);
	printf("---- x86 HEAP Write test: SUCCESS ----\n\n");
	printf("---- x86 HEAP Test: array alloc test ----\n");
	printf("Allocating a character array\n");
	testchar = (char*)malloc(1024*sizeof(char));
	printf("Attempting a write on a character array at address 0x%llx\n",testchar);
	snprintf(testchar, 1024, "%s", "hello");
	printf("Address of char array = 0x%llx , value of char array = %s \n",testchar,testchar);
	printf("---- x86 HEAP Array Write test: SUCCESS ----\n\n");
	printf("---- x86 HEAP memory free test ----\n");
	printf("Attempting to free both the int malloc and char array\n");
	free(testbuffer);
	free(testchar);
	printf("Memory freed\n");
	printf("---- x86 HEAP memory free test: SUCCESS ----\n\n");

	return 0;
}

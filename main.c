#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <CL/cl.h>

// Pointer returned in Output must be freed
size_t LoadTextFile(char **Output, char *Filename)
{
	size_t len;
	FILE *kernel = fopen(Filename, "rb");
	
	fseek(kernel, 0, SEEK_END);
	len = ftell(kernel);
	fseek(kernel, 0, SEEK_SET);
	
	*Output = (char *)malloc(sizeof(char) * (len + 2));
	len = fread(*Output, sizeof(char), len, kernel);
	Output[0][len] = 0x00;		// NULL terminator
	
	return(len);
}
// Parameter len is the size in bytes of asciistr, meaning rawstr
// must have (len >> 1) bytes allocated
// Maybe asciistr just NULL terminated?
// Returns length of rawstr in bytes
int ASCIIHexToBinary(void *restrict rawstr, const char *restrict asciistr, size_t len)
{
	for(int i = 0, j = 0; i < len; ++i)
	{
		char tmp = asciistr[i];
		if(tmp < 'A') tmp -= '0';
		else if(tmp < 'a') tmp = (tmp - 'A') + 10;
		else tmp = (tmp - 'a') + 10;
		
		if(i & 1) ((uint8_t *)rawstr)[j++] |= tmp & 0x0F;
		else ((uint8_t *)rawstr)[j] = tmp << 4;
	}
	
	return(len >> 1);
}

int main(int argc, char **argv)
{
	size_t len;
	cl_int retval;
	cl_context OCLCtx;
	char *KernelSource;
	cl_program Program;
	cl_kernel Kernels[5];
	cl_command_queue Queue;
	cl_uint entries, zero = 0;
	cl_device_id *DeviceIDList, Device;
	uint8_t HashInput[76], HashOutput[32];
	cl_platform_id *PlatformIDList, Platform;
	cl_uint RequestedPlatformIdx = 0, RequestedDeviceIdx = 1;
	cl_mem InputBuf, CNScratchpads, HashStates, BranchNonceBufs[4];
	size_t GlobalThreads = 1, LocalThreads = 1, BranchPathCounts[4];
	const cl_queue_properties CommandQueueProperties[] = { 0, 0, 0 };
	const char *KernelNames[] = { "cryptonight", "Blake", "Groestl", "JH", "Skein" };
	
	if(argc < 2 || (strlen(argv[1]) != 76 * 2))
	{
		printf("Usage: %s <block header hex string>\n", argv[0]);
		return(1);
	}
	
	ASCIIHexToBinary(HashInput, argv[1], 76 << 1);
	
	retval = clGetPlatformIDs(0, NULL, &entries);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clGetPlatformIDs for number of platforms.", retval);
		return(1);
	}
	
	// The number of platforms naturally is the index of the
	// last platform plus one. (Zero exists or the previous
	// call would have failed.)
	if(entries <= RequestedPlatformIdx)
	{
		printf("Selected OpenCL platform index %d doesn't exist.", RequestedPlatformIdx);
		return(1);
	}
	
	PlatformIDList = (cl_platform_id *)malloc(sizeof(cl_platform_id) * entries);
	retval = clGetPlatformIDs(entries, PlatformIDList, NULL);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clGetPlatformIDs for platform ID information.", retval);
		free(PlatformIDList);
		return(1);
	}
	
	Platform = PlatformIDList[RequestedPlatformIdx];
	free(PlatformIDList);
	
	retval = clGetDeviceIDs(Platform, CL_DEVICE_TYPE_GPU, 0, NULL, &entries);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clGetDeviceIDs for number of devices.", retval);
		return(1);
	}
	
	if(entries <= RequestedDeviceIdx)
	{
		printf("Selected OpenCL device index %d doesn't exist.\n", RequestedDeviceIdx);
		return(1);
	}
	
	DeviceIDList = (cl_device_id *)malloc(sizeof(cl_device_id) * entries);
	
	retval = clGetDeviceIDs(Platform, CL_DEVICE_TYPE_GPU, entries, DeviceIDList, NULL);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clGetDeviceIDs for device ID information.\n", retval);
		free(DeviceIDList);
		return(1);
	}
	
	Device = DeviceIDList[RequestedDeviceIdx];
	free(DeviceIDList);
	
	OCLCtx = clCreateContext(NULL, 1, &Device, NULL, NULL, &retval);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clCreateContext.", retval);
		return(1);
	}
	
	Queue = clCreateCommandQueueWithProperties(OCLCtx, Device, CommandQueueProperties, &retval);
	
	InputBuf = clCreateBuffer(OCLCtx, CL_MEM_READ_ONLY, 76, NULL, &retval);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clCreateBuffer to create input buffer.", retval);
		return(1);
	}
	
	CNScratchpads = clCreateBuffer(OCLCtx, CL_MEM_READ_WRITE, GlobalThreads << 21, NULL, &retval);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clCreateBuffer to create hash scratchpads buffer.", retval);
		return(1);
	}
	
	HashStates = clCreateBuffer(OCLCtx, CL_MEM_READ_WRITE, 200 * GlobalThreads, NULL, &retval);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clCreateBuffer to create hash states buffer.", retval);
		return(1);
	}
	
	for(int i = 0; i < 4; ++i)
	{
		BranchNonceBufs[i] = clCreateBuffer(OCLCtx, CL_MEM_READ_WRITE, sizeof(cl_uint) * (GlobalThreads + 2), NULL, &retval);
		
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clCreateBuffer to create Branch %d buffer.", retval, i);
			return(1);
		}
	}
	
	len = LoadTextFile(&KernelSource, "cryptonight.cl");
	
	Program = clCreateProgramWithSource(OCLCtx, 1, (const char **)&KernelSource, NULL, &retval);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clCreateProgramWithSource on the contents of %s.", retval, "cryptonight.cl");
		return(1);
	}
	
	retval = clBuildProgram(Program, 1, &Device, "-I.", NULL, NULL);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clBuildProgram.", retval);
		
		retval = clGetProgramBuildInfo(Program, Device, CL_PROGRAM_BUILD_LOG, 0, NULL, &len);
	
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clGetProgramBuildInfo for length of build log output.", retval);
			return(1);
		}
		
		char *BuildLog = (char *)malloc(sizeof(char) * (len + 2));
		
		retval = clGetProgramBuildInfo(Program, Device, CL_PROGRAM_BUILD_LOG, len, BuildLog, NULL);
		
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clGetProgramBuildInfo for build log.", retval);
			return(1);
		}
		
		printf("Build Log:\n%s", BuildLog);
		
		free(BuildLog);
		
		return(1);
	}
	
	cl_build_status status;
	
	do
	{
		retval = clGetProgramBuildInfo(Program, Device, CL_PROGRAM_BUILD_STATUS, sizeof(cl_build_status), &status, NULL);
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clGetProgramBuildInfo for status of build.", retval);
			return(1);
		}
		
		sleep(1);
	} while(status == CL_BUILD_IN_PROGRESS);
	
	free(KernelSource);
	
	for(int i = 0; i < 5; ++i)
	{
		Kernels[i] = clCreateKernel(Program, KernelNames[i], &retval);
		
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clCreateKernel for kernel %s.", retval, KernelNames[i]);
			return(1);
		}
	}
	
	// Set input data
	retval = clEnqueueWriteBuffer(Queue, InputBuf, CL_TRUE, 0, 76, HashInput, 0, NULL, NULL);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clEnqueueWriteBuffer to fill input buffer.", retval);
		return(1);
	}
	
	// Zero counters
	for(int i = 0; i < 4; ++i)
	{
		retval = clEnqueueWriteBuffer(Queue, BranchNonceBufs[i], CL_TRUE, sizeof(cl_uint) * GlobalThreads, sizeof(cl_uint), &zero, 0, NULL, NULL);
		
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clEnqueueWriteBuffer to zero counter for branch buffer %d.", retval, i);
			return(1);
		}
	}
	
	// Set kernel arguments
	retval = clSetKernelArg(Kernels[0], 0, sizeof(cl_mem), &InputBuf);
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, 0, 0);
		return(1);
	}
	
	retval = clSetKernelArg(Kernels[0], 1, sizeof(cl_mem), &CNScratchpads);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, 0, 1);
		return(1);
	}
	
	retval = clSetKernelArg(Kernels[0], 2, sizeof(cl_mem), &HashStates);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, 0, 2);
		return(1);
	}
	
	for(int i = 3; i < 7; ++i)
	{
		retval = clSetKernelArg(Kernels[0], i, sizeof(cl_mem), BranchNonceBufs + (i - 3));
	
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, 0, i);
			return(1);
		}
	}
	
	retval = clSetKernelArg(Kernels[0], 7, sizeof(cl_ulong), &GlobalThreads);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, 0, 7);
		return(1);
	}
	
	for(int i = 0; i < 4; ++i)
	{
		retval = clSetKernelArg(Kernels[i + 1], 0, sizeof(cl_mem), &HashStates);
		
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, i + 1, 0);
			return(1);
		}
		
		retval = clSetKernelArg(Kernels[i + 1], 1, sizeof(cl_mem), BranchNonceBufs + i);
	
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clSetKernelArg for kernel %d, argument %d.", retval, i + 1, 1);
			return(1);
		}
	}
	
	retval = clEnqueueNDRangeKernel(Queue, Kernels[0], 1, 0, &GlobalThreads, &LocalThreads, 0, NULL, NULL);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clEnqueueNDRangeKernel for kernel %d.", retval, 0);
		return(1);
	}
	
	for(int i = 0; i < 4; ++i)
	{
		retval = clEnqueueReadBuffer(Queue, BranchNonceBufs[i], CL_TRUE, sizeof(cl_uint) * GlobalThreads, sizeof(cl_uint), BranchPathCounts + i, 0, NULL, NULL);
		
		if(retval != CL_SUCCESS)
		{
			printf("Error %d when calling clEnqueueReadBuffer to fetch counter for buffer %d.", retval, i);
			return(1);
		}
	}
	
	for(int i = 0; i < 4; ++i)
	{
		if(BranchPathCounts[i])
		{
			retval = clEnqueueNDRangeKernel(Queue, Kernels[i + 1], 1, 0, BranchPathCounts + i, &LocalThreads, 0, NULL, NULL);
			
			if(retval != CL_SUCCESS)
			{
				printf("Error %d when calling clEnqueueNDRangeKernel for kernel %d.", retval, i + 1);
				return(1);
			}
		}
	}
	
	retval = clEnqueueReadBuffer(Queue, HashStates, CL_TRUE, 0, 32, HashOutput, 0, NULL, NULL);
	
	if(retval != CL_SUCCESS)
	{
		printf("Error %d when calling clEnqueueReadBuffer to fetch results.", retval);
		return(1);
	}
	
	clFinish(Queue);
	
	for(int i = 0; i < 32; ++i) printf("%02X", HashOutput[i]);
	
	putchar('\n');
	
	return(0);
}
	

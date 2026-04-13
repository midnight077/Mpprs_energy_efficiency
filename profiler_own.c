/**
 * Vivek Kumar: Supporting code for PRMP course
 * NOT to be open sourced
 * @TODO: Counter wraps are currently ignored
 *
 * REQUIRED SOFTWARES / STEPS
 * a) sudo apt-get install libpfm4-dev
 * b) if unable to read msr (even with sudo) then use "msr" instead of "msr_safe" inside read_msr()
 * c) sudo apt install linux-tools-common linux-tools-generic linux-tools-$(uname -r)
 * d) echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
 * e) sudo modprob msr
 * f) sudo chmod +rw /dev/cpu/'*'/msr  #Remove the single quotes before running the command
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>

#include <omp.h> //REMOVE IF YOU ARE NOT USING THE PROVIDED FIB EXAMPLE

#define MSR_RAPL_POWER_UNIT_AMD   	0xC0010299
#define MSR_PKG_ENERGY_STATUS_AMD 	0xC001029B  
#define MSR_RAPL_POWER_UNIT_INTEL   	0x606
#define MSR_PKG_ENERGY_STATUS_INTEL 	0x611
#define INTEL	0
#define AMD	1
#define INSTR_EVENT "PERF_COUNT_HW_INSTRUCTIONS"
int PROCESSOR	= -1;
int* instr_fd;
uint64_t* instr_prev, *energy_prev;
int NUM_PHYSICAL_CORES_PER_SOCKET=0, NUM_THREADS_PER_PHYSICAL_CORE=0, NUM_SOCKETS=0, NUM_CORES=0, NUM_LOGICAL_CORES=0;
int* socket_core = NULL; // first core id at each socket
double rapl_joule_unit = 0;
uint64_t* energy_start;
double start_main_timer;
double end_main_timer;

void get_timer(double *timer){
  struct timespec currentTime;
  clock_gettime (CLOCK_MONOTONIC, &currentTime);
  *timer = (currentTime.tv_sec + (currentTime.tv_nsec * 10e-10));
}

// Function to read an MSR register
uint64_t read_msr(int cpu, uint32_t msr) {
    char msr_path[32];
    snprintf(msr_path, sizeof(msr_path), "/dev/cpu/%d/msr", cpu);
    int fd = open(msr_path, O_RDONLY);
    assert(fd >= 0 && "Failed to open MSR file (Try running as sudo)");
    uint64_t value;
    int ret = pread(fd, &value, sizeof(value), msr) != sizeof(value);
    assert(ret != 1 && "Failed to read MSR register");
    close(fd);
    return value;
}

int get_socketID (int cpu) {
  char package_id_path[500];
  FILE *fd;
  int socket;
  sprintf (package_id_path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
  fd = fopen (package_id_path, "r");
  assert(fd!=NULL);
  int rc = fscanf (fd, "%d", &socket);
  assert(rc == 1);
  fclose(fd);
  return socket;
}

void gather_machine_info() {
  FILE *lscpu; char buffer[256];
  int num_cpus_from_lscpu = 0;
  int cpu_info[4] = {0};
  __asm__ volatile (
      "cpuid"
      : "=b" (cpu_info[0]), "=d" (cpu_info[1]), "=c" (cpu_info[2])
      : "a" (0)
  );
  if (*(int*)&cpu_info[0] == 0x756e6547) {
    PROCESSOR = INTEL;
  } else if (*(int*)&cpu_info[0] == 0x68747541) {
    PROCESSOR = AMD;
  } else {
    assert(0 && "Unknown CPU type");
  }
  lscpu = popen("lscpu", "r");
  assert(lscpu != NULL && "Cannot execute lscpu");
  while (fgets(buffer, sizeof(buffer), lscpu)) {
    printf("1  ");
    if (strncmp(buffer, "CPU(s):", 7) == 0) {
      sscanf(buffer, "CPU(s): %d", &num_cpus_from_lscpu);
    } else if (strncmp(buffer, "Core(s) per socket:", 19) == 0) {
      sscanf(buffer, "Core(s) per socket: %d", &NUM_PHYSICAL_CORES_PER_SOCKET);
    } else if (strncmp(buffer, "Thread(s) per core:", 19) == 0) {
      sscanf(buffer, "Thread(s) per core: %d" ,&NUM_THREADS_PER_PHYSICAL_CORE);
    } else if (strncmp(buffer, "Socket(s):", 10) == 0) {
      sscanf(buffer, "Socket(s): %d", &NUM_SOCKETS);
      printf("%d",NUM_SOCKETS);
    }
  }
  pclose(lscpu);
  assert(NUM_PHYSICAL_CORES_PER_SOCKET>0 && NUM_THREADS_PER_PHYSICAL_CORE>0 && NUM_SOCKETS>0 && "Incorrect machine information");
  NUM_CORES = NUM_PHYSICAL_CORES_PER_SOCKET*NUM_SOCKETS;
  NUM_LOGICAL_CORES = NUM_CORES*NUM_THREADS_PER_PHYSICAL_CORE;
  if (num_cpus_from_lscpu > 0) {
    NUM_LOGICAL_CORES = num_cpus_from_lscpu;
  }
  {
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0 && online < NUM_LOGICAL_CORES) {
      NUM_LOGICAL_CORES = (int)online;
    }
  }

  // Determine first physical core at each socket
  socket_core = (int*) malloc(sizeof(int) * NUM_SOCKETS);
  for(int c=0, curr_socket=-1; c<NUM_CORES; c++) {
    const int sock = get_socketID(c);
    if(sock>curr_socket) {
      curr_socket=sock;
      socket_core[curr_socket]=c;
    }
    if(curr_socket == (NUM_SOCKETS-1)) break;
  }
}

void register_perf_event(int core) {
  struct perf_event_attr attr;
  int ret;
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  ret = pfm_get_perf_event_encoding(INSTR_EVENT, PFM_PLM0 | PFM_PLM3, &attr, NULL, NULL);
  if (ret != PFM_SUCCESS) {
    fprintf(stderr, "pfm_get_perf_event_encoding failed on core %d: %s\n", core, pfm_strerror(ret));
    assert(0 && "Failed in pfm_get_perf_event_encoding");
  }
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
  attr.disabled = 1;
  errno = 0;
  instr_fd[core] = perf_event_open(&attr, -1, core, -1, 0); // Open event for all processes/threads at 'core'
  if(instr_fd[core] < 0) {
    fprintf(stderr,
            "perf_event_open failed on core %d (type=%u, config=0x%llx): errno=%d (%s)\n",
            core,
            attr.type,
            (unsigned long long)attr.config,
            errno,
            strerror(errno));
    if (errno == EACCES || errno == EPERM) {
      fprintf(stderr, "Hint: use sudo/CAP_PERFMON and set /proc/sys/kernel/perf_event_paranoid to <= 1.\n");
    }
    assert(0 && "Failed to create event");
  }
  // Activates the performance counter associated with file descriptor fd[i]
  ret = ioctl(instr_fd[core], PERF_EVENT_IOC_ENABLE, 0);
  if(ret) assert(0 && "Failed to enable counter using ioctl");
}

double read_energy(int record_init, int record_finalize) {
  double diff = 0;
  for(int i=0; i<NUM_SOCKETS; i++) {
    uint64_t energy;
    if(PROCESSOR == AMD) { 
      energy = read_msr(socket_core[i], MSR_PKG_ENERGY_STATUS_AMD);
    } else {
      energy = read_msr(socket_core[i], MSR_PKG_ENERGY_STATUS_INTEL);
    }
    if(record_finalize) {
      diff += energy - energy_start[i];
    } else {
      diff += energy - energy_prev[i];
    }
    energy_prev[i] = energy;
    if(record_init) energy_start[i] = energy;
  }
  return (diff * rapl_joule_unit);
}

uint64_t read_instr() {
  uint64_t diff=0;
  for(int i=0; i<NUM_LOGICAL_CORES; i++) {
    uint64_t values[3] = {0}, val = 0;
    int ret = read(instr_fd[i], values, sizeof(values));
    if (ret != sizeof(values)) assert(0 && "counter reading failed");
    if (values[2]) val = (uint64_t)((double)values[0] * values[1] / values[2]);
    diff += (val - instr_prev[i]);
    instr_prev[i] = val;
  }
  return diff;
}

double calculate_JPI() {
  return (read_energy(0, 0) / ((double) read_instr()));
}

void profiler_init() {
  gather_machine_info();
  uint64_t rapl_power_unit = 0;
  if(PROCESSOR == AMD) { 
    rapl_power_unit = read_msr(0, MSR_RAPL_POWER_UNIT_AMD);
  } else {
    rapl_power_unit = read_msr(0, MSR_RAPL_POWER_UNIT_INTEL);
  }
  assert(rapl_power_unit>0);
  rapl_joule_unit = 1.0 / (1 << ((rapl_power_unit >> 8) & 0x1F)); 
  assert(pfm_initialize() == PFM_SUCCESS && "pfm_initialize Failed");
  instr_fd = (int*) malloc(sizeof(int) * NUM_LOGICAL_CORES);
  instr_prev = (uint64_t*) malloc(sizeof(uint64_t) * NUM_LOGICAL_CORES);
  energy_prev = (uint64_t*) malloc(sizeof(uint64_t) * NUM_SOCKETS);
  energy_start = (uint64_t*) malloc(sizeof(uint64_t) * NUM_SOCKETS);
  for(int i=0; i<NUM_LOGICAL_CORES; i++) {
    register_perf_event(i);
  }
  get_timer(&start_main_timer);
  read_instr();
  read_energy(1, 0);
}

void profiler_finalize() {
  double energy = read_energy(0, 1);
  get_timer(&end_main_timer);
  double time = end_main_timer - start_main_timer;
  fprintf(stdout,"\n============================ Tabulate Statistics ============================\n");
  fprintf(stdout,"TIME(sec)\tENERGY(Joules)\tEDP(Lower-the-Better)\n");
  fprintf(stdout,"%.3f\t%.3f\t%.3f",time, energy, time*energy);
  fprintf(stdout,"\n=============================================================================\n");
  fflush(stdout); 
  free(instr_fd);
  free(instr_prev);
  free(energy_prev);
  free(energy_start);
}

// int fib(int n) {
//   if(n<2) return n;
//   int x, y;
//   #pragma omp task untied shared(x)
//   x = fib(n-1);
//   #pragma omp task untied shared(y)
//   y = fib(n-2);
//   #pragma omp taskwait
//   return x+y;
// }

// int main () {
//   profiler_init();      // SHOULD BE CALLED IMMEDIATELY WHEN ENTERING MAIN
//   for(int i=0; i <10; i++) {
//     printf("Calling fib(25) %dth time\n",i);
//     #pragma omp parallel
//     #pragma omp single
//     fib(25);
//     printf("JPI = %.12f\n", calculate_JPI());
//   }
//   profiler_finalize();  // SHOULD BE CALLED IMMEDIATELY BEFORE EXITING MAIN
//   return 0;
// }


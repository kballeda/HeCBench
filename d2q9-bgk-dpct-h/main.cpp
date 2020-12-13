/*
 ** Code to implement a d2q9-bgk lattice boltzmann scheme.
 ** 'd2' inidates a 2-dimensional grid, and
 ** 'q9' indicates 9 velocities per grid cell.
 ** 'bgk' refers to the Bhatnagar-Gross-Krook collision step.
 **
 ** The 'speeds' in each cell are numbered as follows:
 **
 ** 6 2 5
 **  \|/
 ** 3-0-1
 **  /|\
 ** 7 4 8
 **
 ** A 2D grid:
 **
 **           cols
 **       --- --- ---
 **      | D | E | F |
 ** rows  --- --- ---
 **      | A | B | C |
 **       --- --- ---
 **
 ** 'unwrapped' in row major order to give a 1D array:
 **
 **  --- --- --- --- --- ---
 ** | A | B | C | D | E | F |
 **  --- --- --- --- --- ---
 **
 ** Grid indicies are:
 **
 **          ny
 **          ^       cols(ii)
 **          |  ----- ----- -----
 **          | | ... | ... | etc |
 **          |  ----- ----- -----
 ** rows(jj) | | 1,0 | 1,1 | 1,2 |
 **          |  ----- ----- -----
 **          | | 0,0 | 0,1 | 0,2 |
 **          |  ----- ----- -----
 **          ----------------------> nx
 **
 ** Note the names of the input parameter and obstacle files
 ** are passed on the command line, e.g.:
 **
 **   ./d2q9-bgk input.params obstacles.dat
 **
 ** Be sure to adjust the grid dimensions in the parameter file
 ** if you choose a different obstacle file.
 */

#define DPCT_USM_LEVEL_NONE
#include <CL/sycl.hpp>
#include <dpct/dpct.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>

#define NSPEEDS         9
#define LOCALSIZEX      128
#define LOCALSIZEY      1

/* dump output files for verification */
#define FINALSTATEFILE  "final_state.dat"
#define AVVELSFILE      "av_vels.dat"

/* struct to hold the parameter values */
typedef struct dpct_type_db7871
{
  int   nx;            /* no. of cells in x-direction */
  int   ny;            /* no. of cells in y-direction */
  int   maxIters;      /* no. of iterations */
  int   reynolds_dim;  /* dimension for Reynolds number */
  float density;       /* density per link */
  float accel;         /* density redistribution */
  float omega;         /* relaxation parameter */
} t_param;

/* struct to hold the 'speed' values */
typedef struct dpct_type_f1dc27
{
  float speeds[NSPEEDS];
} t_speed;

/*
 ** function prototypes
 */

/* load params, allocate memory, load obstacles & initialise fluid particle densities */
int initialise(const char* paramfile, const char* obstaclefile,
    t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
    int** obstacles_ptr, float** av_vels_ptr);

/*
 ** The main calculation methods.
 ** timestep calls, in order, the functions:
 ** accelerate_flow(), propagate(), rebound() & collision()
 */
int write_values(const t_param params, t_speed* cells, int* obstacles, float* av_vels);

/* finalise, including freeing up allocated memory */
int finalise(t_speed* cells_ptr, t_speed* tmp_cells_ptr,
    int* obstacles_ptr, float* av_vels_ptr);

/* Sum all the densities in the grid.
 ** The total should remain constant from one timestep to the next. */
float total_density(const t_param params, t_speed* cells);

/* compute average velocity */
float av_velocity(const t_param params, t_speed* cells, int* obstacles);

/* calculate Reynolds number */
float calc_reynolds(const t_param params, t_speed* cells, int* obstacles);

/* utility functions */
void die(const char* message, const int line, const char* file);
void usage(const char* exe);

bool 
isGreater(const float x, const float y) 
{
  return x > y ? 1 : 0;
}

void
d2q9_bgk( float* Speed0A,
   const float* Speed1A,
   const float* Speed2A,
   const float* Speed3A,
   const float* Speed4A,
   const float* Speed5A,
   const float* Speed6A,
   const float* Speed7A,
   const float* Speed8A,
   float* Tmp0A,
   float* Tmp1A,
   float* Tmp2A,
   float* Tmp3A,
   float* Tmp4A,
   float* Tmp5A,
   float* Tmp6A,
   float* Tmp7A,
   float* Tmp8A,
   const int* ObstaclesA,
   float* Partial_Sum,
   int* Partial_Sum2,
   const float densityaccel,
   const float omega,
   const int nx,
   const int ny,
   const int tt,
   sycl::nd_item<3> item_ct1,
   int *local_sum2,
   float *local_sum)
{

  //setup local memory

  /* get column and row indices */
  const int ii = item_ct1.get_group(2) * item_ct1.get_local_range().get(2) +
                 item_ct1.get_local_id(2);
  const int jj = item_ct1.get_group(1) * item_ct1.get_local_range().get(1) +
                 item_ct1.get_local_id(1);

  const float c_sq_inv = 3.f;
  const float c_sq = 1.f/c_sq_inv; /* square of speed of sound */
  const float temp1 = 4.5f;
  const float w1 = 1.f/9.f;
  const float w0 = 4.f * w1;  /* weighting factor */
  const float w2 = 1.f/36.f; /* weighting factor */
  const float w11 = densityaccel * w1;
  const float w21 = densityaccel * w2;

  /* determine indices of axis-direction neighbours
   ** respecting periodic boundary conditions (wrap around) */
  const int y_n = (jj + 1) % ny;
  const int x_e = (ii + 1) % nx;
  const int y_s = (jj == 0) ? (jj + ny - 1) : (jj - 1);
  const int x_w = (ii == 0) ? (ii + nx - 1) : (ii - 1);

  /* propagate densities from neighbouring cells, following
   ** appropriate directions of travel and writing into
   ** scratch space grid */

  float tmp_s0 = Speed0A[ii + jj*nx];
  float tmp_s1 = (jj == ny-2 && (!ObstaclesA[x_w + jj*nx] && isGreater((Speed3A[x_w + jj*nx] - w11) , 0.f) && isGreater((Speed6A[x_w + jj*nx] - w21) , 0.f) && isGreater((Speed7A[x_w + jj*nx] - w21) , 0.f))) ? Speed1A[x_w + jj*nx]+w11 : Speed1A[x_w + jj*nx];
  float tmp_s2 = Speed2A[ii + y_s*nx];
  float tmp_s3 = (jj == ny-2 && (!ObstaclesA[x_e + jj*nx] && isGreater((Speed3A[x_e + jj*nx] - w11) , 0.f) && isGreater((Speed6A[x_e + jj*nx] - w21) , 0.f) && isGreater((Speed7A[x_e + jj*nx] - w21) , 0.f))) ? Speed3A[x_e + jj*nx]-w11 : Speed3A[x_e + jj*nx];
  float tmp_s4 = Speed4A[ii + y_n*nx];
  float tmp_s5 = (y_s == ny-2 && (!ObstaclesA[x_w + y_s*nx] && isGreater((Speed3A[x_w + y_s*nx] - w11) , 0.f) && isGreater((Speed6A[x_w + y_s*nx] - w21) , 0.f) && isGreater((Speed7A[x_w + y_s*nx] - w21) , 0.f))) ? Speed5A[x_w + y_s*nx]+w21 : Speed5A[x_w + y_s*nx];
  float tmp_s6 = (y_s == ny-2 && (!ObstaclesA[x_e + y_s*nx] && isGreater((Speed3A[x_e + y_s*nx] - w11) , 0.f) && isGreater((Speed6A[x_e + y_s*nx] - w21) , 0.f) && isGreater((Speed7A[x_e + y_s*nx] - w21) , 0.f))) ? Speed6A[x_e + y_s*nx]-w21 : Speed6A[x_e + y_s*nx];
  float tmp_s7 = (y_n == ny-2 && (!ObstaclesA[x_e + y_n*nx] && isGreater((Speed3A[x_e + y_n*nx] - w11) , 0.f) && isGreater((Speed6A[x_e + y_n*nx] - w21) , 0.f) && isGreater((Speed7A[x_e + y_n*nx] - w21) , 0.f))) ? Speed7A[x_e + y_n*nx]-w21 : Speed7A[x_e + y_n*nx];
  float tmp_s8 = (y_n == ny-2 && (!ObstaclesA[x_w + y_n*nx] && isGreater((Speed3A[x_w + y_n*nx] - w11) , 0.f) && isGreater((Speed6A[x_w + y_n*nx] - w21) , 0.f) && isGreater((Speed7A[x_w + y_n*nx] - w21) , 0.f))) ? Speed8A[x_w + y_n*nx]+w21 : Speed8A[x_w + y_n*nx];

  /* compute local density total */
  float local_density = tmp_s0 + tmp_s1 + tmp_s2 + tmp_s3 + tmp_s4  + tmp_s5  + tmp_s6  + tmp_s7  + tmp_s8;
  const float local_density_recip = 1.f/(local_density);
  /* compute x velocity component */
  float u_x = (tmp_s1
      + tmp_s5
      + tmp_s8
      - tmp_s3
      - tmp_s6
      - tmp_s7)
    * local_density_recip;
  /* compute y velocity component */
  float u_y = (tmp_s2
      + tmp_s5
      + tmp_s6
      - tmp_s4
      - tmp_s8
      - tmp_s7)
    * local_density_recip;

  /* velocity squared */
  const float temp2 = - (u_x * u_x + u_y * u_y)/(2.f * c_sq);

  /* equilibrium densities */
  float d_equ[NSPEEDS];
  /* zero velocity density: weight w0 */
  d_equ[0] = w0 * local_density
    * (1.f + temp2);
  /* axis speeds: weight w1 */
  d_equ[1] = w1 * local_density * (1.f + u_x * c_sq_inv
      + (u_x * u_x) * temp1
      + temp2);
  d_equ[2] = w1 * local_density * (1.f + u_y * c_sq_inv
      + (u_y * u_y) * temp1
      + temp2);
  d_equ[3] = w1 * local_density * (1.f - u_x * c_sq_inv
      + (u_x * u_x) * temp1
      + temp2);
  d_equ[4] = w1 * local_density * (1.f - u_y * c_sq_inv
      + (u_y * u_y) * temp1
      + temp2);
  /* diagonal speeds: weight w2 */
  d_equ[5] = w2 * local_density * (1.f + (u_x + u_y) * c_sq_inv
      + ((u_x + u_y) * (u_x + u_y)) * temp1
      + temp2);
  d_equ[6] = w2 * local_density * (1.f + (-u_x + u_y) * c_sq_inv
      + ((-u_x + u_y) * (-u_x + u_y)) * temp1
      + temp2);
  d_equ[7] = w2 * local_density * (1.f + (-u_x - u_y) * c_sq_inv
      + ((-u_x - u_y) * (-u_x - u_y)) * temp1
      + temp2);
  d_equ[8] = w2 * local_density * (1.f + (u_x - u_y) * c_sq_inv
      + ((u_x - u_y) * (u_x - u_y)) * temp1
      + temp2);

  float tmp;
  int expression = ObstaclesA[ii + jj*nx];
  tmp_s0 = expression ? tmp_s0 : (tmp_s0 + omega * (d_equ[0] - tmp_s0));
  tmp = tmp_s1;
  tmp_s1 = expression ? tmp_s3 : (tmp_s1 + omega * (d_equ[1] - tmp_s1));
  tmp_s3 = expression ? tmp : (tmp_s3 + omega * (d_equ[3] - tmp_s3));
  tmp = tmp_s2;
  tmp_s2 = expression ? tmp_s4 : (tmp_s2 + omega * (d_equ[2] - tmp_s2));
  tmp_s4 = expression ? tmp : (tmp_s4 + omega * (d_equ[4] - tmp_s4));
  tmp = tmp_s5;
  tmp_s5 = expression ? tmp_s7 : (tmp_s5 + omega * (d_equ[5] - tmp_s5));
  tmp_s7 = expression ? tmp : (tmp_s7 + omega * (d_equ[7] - tmp_s7));
  tmp = tmp_s6;
  tmp_s6 = expression ? tmp_s8 : (tmp_s6 + omega * (d_equ[6] - tmp_s6));
  tmp_s8 = expression ? tmp : (tmp_s8 + omega * (d_equ[8] - tmp_s8));

  /* local density total */
  local_density = 1.f/(tmp_s0 + tmp_s1 + tmp_s2 + tmp_s3 + tmp_s4 + tmp_s5 + tmp_s6 + tmp_s7 + tmp_s8);

  /* x-component of velocity */
  u_x = (tmp_s1
      + tmp_s5
      + tmp_s8
      - tmp_s3
      - tmp_s6
      - tmp_s7)
    * local_density;
  /* compute y velocity component */
  u_y = (tmp_s2
      + tmp_s5
      + tmp_s6
      - tmp_s4
      - tmp_s7
      - tmp_s8)
    * local_density;

  Tmp0A[ii + jj*nx] = tmp_s0;
  Tmp1A[ii + jj*nx] = tmp_s1;
  Tmp2A[ii + jj*nx] = tmp_s2;
  Tmp3A[ii + jj*nx] = tmp_s3;
  Tmp4A[ii + jj*nx] = tmp_s4;
  Tmp5A[ii + jj*nx] = tmp_s5;
  Tmp6A[ii + jj*nx] = tmp_s6;
  Tmp7A[ii + jj*nx] = tmp_s7;
  Tmp8A[ii + jj*nx] = tmp_s8;

  int local_idi = item_ct1.get_local_id(2);
  int local_idj = item_ct1.get_local_id(1);
  int local_sizei = item_ct1.get_local_range().get(2);
  int local_sizej = item_ct1.get_local_range().get(1);

  /* accumulate the norm of x- and y- velocity components */
  local_sum[local_idi + local_idj * local_sizei] =
      (ObstaclesA[ii + jj * nx]) ? 0 : sycl::hypot(u_x, u_y);
  /* increase counter of inspected cells */
  local_sum2[local_idi + local_idj*local_sizei] = (ObstaclesA[ii + jj*nx]) ? 0 : 1 ;

  item_ct1.barrier();

  int group_id = item_ct1.get_group(2);
  int group_id2 = item_ct1.get_group(1);
  int group_size = item_ct1.get_group_range(2);
  int group_size2 = item_ct1.get_group_range(1);
  if(local_idi == 0 && local_idj == 0){
    float sum = 0.0f;
    int sum2 = 0;
    for(int i = 0; i<local_sizei*local_sizej; i++){
      sum += local_sum[i];
      sum2 += local_sum2[i];
    }
    Partial_Sum[group_id+group_id2*group_size+tt*group_size*group_size2] = sum;
    Partial_Sum2[group_id+group_id2*group_size+tt*group_size*group_size2] = sum2;
  }
}

int main(int argc, char* argv[])
{
  char*    paramfile = NULL;    /* input parameter file */
  char*    obstaclefile = NULL; /* input obstacle file */
  t_param  params;              /* struct to hold parameter values */
  t_speed* cells     = NULL;    /* grid containing fluid densities */
  t_speed* tmp_cells = NULL;    /* scratch space */
  int*     obstaclesHost = NULL;/* grid indicating which cells are blocked */
  float*   av_vels   = NULL;    /* a record of the av. velocity computed for each timestep */
  struct timeval timstr;        /* structure to hold elapsed time */
  double tic, toc;              /* floating point numbers to calculate elapsed wallclock time */

  /* parse the command line */
  if (argc != 3)
  {
    usage(argv[0]);
  }
  else
  {
    paramfile = argv[1];
    obstaclefile = argv[2];
  }

  /* initialise our data structures and load values from file */
  initialise(paramfile, obstaclefile, &params, &cells, 
      &tmp_cells, &obstaclesHost, &av_vels);

  // declare host arrays
  int Ny = params.ny;
  int Nx = params.nx;
  int MaxIters = params.maxIters;

  float *speedsHostS0 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS1 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS2 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS3 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS4 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS5 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS6 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS7 = (float*) malloc (sizeof(float)*Ny*Nx);
  float *speedsHostS8 = (float*) malloc (sizeof(float)*Ny*Nx);

  float *tot_up = (float*) malloc (sizeof(float) * (Ny/LOCALSIZEY) * (Nx/LOCALSIZEX) * MaxIters);
  int *tot_cellsp = (int*) malloc (sizeof(int) * (Ny/LOCALSIZEY) * (Nx/LOCALSIZEX) * MaxIters);

  // Init arrays
  /* loop over _all_ cells */
  for (int jj = 0; jj < Ny; jj++)
  {
    for (int ii = 0; ii < Nx; ii++)
    {
      speedsHostS0[ii + jj*Nx] = cells[ii + jj*Nx].speeds[0];
      speedsHostS1[ii + jj*Nx] = cells[ii + jj*Nx].speeds[1];
      speedsHostS2[ii + jj*Nx] = cells[ii + jj*Nx].speeds[2];
      speedsHostS3[ii + jj*Nx] = cells[ii + jj*Nx].speeds[3];
      speedsHostS4[ii + jj*Nx] = cells[ii + jj*Nx].speeds[4];
      speedsHostS5[ii + jj*Nx] = cells[ii + jj*Nx].speeds[5];
      speedsHostS6[ii + jj*Nx] = cells[ii + jj*Nx].speeds[6];
      speedsHostS7[ii + jj*Nx] = cells[ii + jj*Nx].speeds[7];
      speedsHostS8[ii + jj*Nx] = cells[ii + jj*Nx].speeds[8];
    }
  }


  //start timer
  gettimeofday(&timstr, NULL);
  tic = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  // Creating buffers which are bound to host arrays
  float *speeds0, *speeds1, *speeds2, *speeds3, *speeds4,
    *speeds5, *speeds6, *speeds7, *speeds8;

  float *tmp_speeds0, *tmp_speeds1, *tmp_speeds2, *tmp_speeds3, *tmp_speeds4,
    *tmp_speeds5, *tmp_speeds6, *tmp_speeds7, *tmp_speeds8;

  dpct::dpct_malloc((void **)&speeds0, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds0, speedsHostS0, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds1, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds1, speedsHostS1, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds2, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds2, speedsHostS2, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds3, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds3, speedsHostS3, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds4, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds4, speedsHostS4, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds5, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds5, speedsHostS5, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds6, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds6, speedsHostS6, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds7, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds7, speedsHostS7, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);
  dpct::dpct_malloc((void **)&speeds8, sizeof(float) * Ny * Nx);
  dpct::dpct_memcpy(speeds8, speedsHostS8, sizeof(float) * Ny * Nx,
                    dpct::host_to_device);

  dpct::dpct_malloc((void **)&tmp_speeds0, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds1, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds2, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds3, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds4, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds5, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds6, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds7, sizeof(float) * Ny * Nx);
  dpct::dpct_malloc((void **)&tmp_speeds8, sizeof(float) * Ny * Nx);

  int *obstacles, *partial_sum2; 
  float *partial_sum;

  dpct::dpct_malloc((void **)&obstacles, sizeof(int) * Ny * Nx);
  dpct::dpct_malloc((void **)&partial_sum, sizeof(float) * (Ny / LOCALSIZEY) *
                                               (Nx / LOCALSIZEX) * MaxIters);
  dpct::dpct_malloc((void **)&partial_sum2, sizeof(int) * (Ny / LOCALSIZEY) *
                                                (Nx / LOCALSIZEX) * MaxIters);

  dpct::dpct_memcpy(obstacles, obstaclesHost, sizeof(int) * Ny * Nx,
                    dpct::host_to_device);

  // parameters for kernel 
  float omega = params.omega;
  float densityaccel = params.density*params.accel;

  sycl::range<3> grids(Nx / LOCALSIZEX, Ny / LOCALSIZEY, 1);
  sycl::range<3> threads(LOCALSIZEX, LOCALSIZEY, 1);

  for (int tt = 0; tt < MaxIters; tt++){
    {
      dpct::buffer_t speeds0_buf_ct0 = dpct::get_buffer(speeds0);
      dpct::buffer_t speeds1_buf_ct1 = dpct::get_buffer(speeds1);
      dpct::buffer_t speeds2_buf_ct2 = dpct::get_buffer(speeds2);
      dpct::buffer_t speeds3_buf_ct3 = dpct::get_buffer(speeds3);
      dpct::buffer_t speeds4_buf_ct4 = dpct::get_buffer(speeds4);
      dpct::buffer_t speeds5_buf_ct5 = dpct::get_buffer(speeds5);
      dpct::buffer_t speeds6_buf_ct6 = dpct::get_buffer(speeds6);
      dpct::buffer_t speeds7_buf_ct7 = dpct::get_buffer(speeds7);
      dpct::buffer_t speeds8_buf_ct8 = dpct::get_buffer(speeds8);
      dpct::buffer_t tmp_speeds0_buf_ct9 = dpct::get_buffer(tmp_speeds0);
      dpct::buffer_t tmp_speeds1_buf_ct10 = dpct::get_buffer(tmp_speeds1);
      dpct::buffer_t tmp_speeds2_buf_ct11 = dpct::get_buffer(tmp_speeds2);
      dpct::buffer_t tmp_speeds3_buf_ct12 = dpct::get_buffer(tmp_speeds3);
      dpct::buffer_t tmp_speeds4_buf_ct13 = dpct::get_buffer(tmp_speeds4);
      dpct::buffer_t tmp_speeds5_buf_ct14 = dpct::get_buffer(tmp_speeds5);
      dpct::buffer_t tmp_speeds6_buf_ct15 = dpct::get_buffer(tmp_speeds6);
      dpct::buffer_t tmp_speeds7_buf_ct16 = dpct::get_buffer(tmp_speeds7);
      dpct::buffer_t tmp_speeds8_buf_ct17 = dpct::get_buffer(tmp_speeds8);
      dpct::buffer_t obstacles_buf_ct18 = dpct::get_buffer(obstacles);
      dpct::buffer_t partial_sum_buf_ct19 = dpct::get_buffer(partial_sum);
      dpct::buffer_t partial_sum2_buf_ct20 = dpct::get_buffer(partial_sum2);
      dpct::get_default_queue().submit([&](sycl::handler &cgh) {
        sycl::accessor<int, 1, sycl::access::mode::read_write,
                       sycl::access::target::local>
            local_sum2_acc_ct1(sycl::range<1>(128 /*LOCALSIZEX*LOCALSIZEY*/),
                               cgh);
        sycl::accessor<float, 1, sycl::access::mode::read_write,
                       sycl::access::target::local>
            local_sum_acc_ct1(sycl::range<1>(128 /*LOCALSIZEX*LOCALSIZEY*/),
                              cgh);
        auto speeds0_acc_ct0 =
            speeds0_buf_ct0.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds1_acc_ct1 =
            speeds1_buf_ct1.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds2_acc_ct2 =
            speeds2_buf_ct2.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds3_acc_ct3 =
            speeds3_buf_ct3.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds4_acc_ct4 =
            speeds4_buf_ct4.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds5_acc_ct5 =
            speeds5_buf_ct5.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds6_acc_ct6 =
            speeds6_buf_ct6.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds7_acc_ct7 =
            speeds7_buf_ct7.get_access<sycl::access::mode::read_write>(cgh);
        auto speeds8_acc_ct8 =
            speeds8_buf_ct8.get_access<sycl::access::mode::read_write>(cgh);
        auto tmp_speeds0_acc_ct9 =
            tmp_speeds0_buf_ct9.get_access<sycl::access::mode::read_write>(cgh);
        auto tmp_speeds1_acc_ct10 =
            tmp_speeds1_buf_ct10.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds2_acc_ct11 =
            tmp_speeds2_buf_ct11.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds3_acc_ct12 =
            tmp_speeds3_buf_ct12.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds4_acc_ct13 =
            tmp_speeds4_buf_ct13.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds5_acc_ct14 =
            tmp_speeds5_buf_ct14.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds6_acc_ct15 =
            tmp_speeds6_buf_ct15.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds7_acc_ct16 =
            tmp_speeds7_buf_ct16.get_access<sycl::access::mode::read_write>(
                cgh);
        auto tmp_speeds8_acc_ct17 =
            tmp_speeds8_buf_ct17.get_access<sycl::access::mode::read_write>(
                cgh);
        auto obstacles_acc_ct18 =
            obstacles_buf_ct18.get_access<sycl::access::mode::read_write>(cgh);
        auto partial_sum_acc_ct19 =
            partial_sum_buf_ct19.get_access<sycl::access::mode::read_write>(
                cgh);
        auto partial_sum2_acc_ct20 =
            partial_sum2_buf_ct20.get_access<sycl::access::mode::read_write>(
                cgh);

        auto dpct_global_range = grids * threads;

        cgh.parallel_for(
            sycl::nd_range<3>(
                sycl::range<3>(dpct_global_range.get(2),
                               dpct_global_range.get(1),
                               dpct_global_range.get(0)),
                sycl::range<3>(threads.get(2), threads.get(1), threads.get(0))),
            [=](sycl::nd_item<3> item_ct1) {
              d2q9_bgk((float *)(&speeds0_acc_ct0[0]),
                       (const float *)(&speeds1_acc_ct1[0]),
                       (const float *)(&speeds2_acc_ct2[0]),
                       (const float *)(&speeds3_acc_ct3[0]),
                       (const float *)(&speeds4_acc_ct4[0]),
                       (const float *)(&speeds5_acc_ct5[0]),
                       (const float *)(&speeds6_acc_ct6[0]),
                       (const float *)(&speeds7_acc_ct7[0]),
                       (const float *)(&speeds8_acc_ct8[0]),
                       (float *)(&tmp_speeds0_acc_ct9[0]),
                       (float *)(&tmp_speeds1_acc_ct10[0]),
                       (float *)(&tmp_speeds2_acc_ct11[0]),
                       (float *)(&tmp_speeds3_acc_ct12[0]),
                       (float *)(&tmp_speeds4_acc_ct13[0]),
                       (float *)(&tmp_speeds5_acc_ct14[0]),
                       (float *)(&tmp_speeds6_acc_ct15[0]),
                       (float *)(&tmp_speeds7_acc_ct16[0]),
                       (float *)(&tmp_speeds8_acc_ct17[0]),
                       (const int *)(&obstacles_acc_ct18[0]),
                       (float *)(&partial_sum_acc_ct19[0]),
                       (int *)(&partial_sum2_acc_ct20[0]), densityaccel, omega,
                       Nx, Ny, tt, item_ct1, local_sum2_acc_ct1.get_pointer(),
                       local_sum_acc_ct1.get_pointer());
            });
      });
    }

    // swap the buffers
    float* speed_tmp = speeds0;
    speeds0 = tmp_speeds0;
    tmp_speeds0 = speed_tmp;

    speed_tmp = speeds1;
    speeds1 = tmp_speeds1;
    tmp_speeds1 = speed_tmp;

    speed_tmp = speeds2;
    speeds2 = tmp_speeds2;
    tmp_speeds2 = speed_tmp;

    speed_tmp = speeds3;
    speeds3 = tmp_speeds3;
    tmp_speeds3 = speed_tmp;

    speed_tmp = speeds4;
    speeds4 = tmp_speeds4;
    tmp_speeds4 = speed_tmp;

    speed_tmp = speeds5;
    speeds5 = tmp_speeds5;
    tmp_speeds5 = speed_tmp;

    speed_tmp = speeds6;
    speeds6 = tmp_speeds6;
    tmp_speeds6 = speed_tmp;

    speed_tmp = speeds7;
    speeds7 = tmp_speeds7;
    tmp_speeds7 = speed_tmp;

    speed_tmp = speeds8;
    speeds8 = tmp_speeds8;
    tmp_speeds8 = speed_tmp;
  }

  dpct::dpct_memcpy(tot_up, partial_sum,
                    sizeof(float) * (Ny / LOCALSIZEY) * (Nx / LOCALSIZEX) *
                        MaxIters,
                    dpct::device_to_host);
  dpct::dpct_memcpy(tot_cellsp, partial_sum2,
                    sizeof(int) * (Ny / LOCALSIZEY) * (Nx / LOCALSIZEX) *
                        MaxIters,
                    dpct::device_to_host);

  dpct::dpct_memcpy(speedsHostS0, speeds0, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS1, speeds1, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS2, speeds2, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS3, speeds3, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS4, speeds4, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS5, speeds5, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS6, speeds6, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS7, speeds7, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);
  dpct::dpct_memcpy(speedsHostS8, speeds8, sizeof(float) * Ny * Nx,
                    dpct::device_to_host);

  dpct::dpct_free(speeds0);
  dpct::dpct_free(speeds1);
  dpct::dpct_free(speeds2);
  dpct::dpct_free(speeds3);
  dpct::dpct_free(speeds4);
  dpct::dpct_free(speeds5);
  dpct::dpct_free(speeds6);
  dpct::dpct_free(speeds7);
  dpct::dpct_free(speeds8);
  dpct::dpct_free(tmp_speeds0);
  dpct::dpct_free(tmp_speeds1);
  dpct::dpct_free(tmp_speeds2);
  dpct::dpct_free(tmp_speeds3);
  dpct::dpct_free(tmp_speeds4);
  dpct::dpct_free(tmp_speeds5);
  dpct::dpct_free(tmp_speeds6);
  dpct::dpct_free(tmp_speeds7);
  dpct::dpct_free(tmp_speeds8);
  dpct::dpct_free(obstacles);
  dpct::dpct_free(partial_sum2);
  dpct::dpct_free(partial_sum);

  float tot_u = 0;
  int tot_cells = 0;
  for (int tt = 0; tt < params.maxIters; tt++){
    tot_u = 0;
    tot_cells = 0;
    for(int i = 0; i < params.nx/LOCALSIZEX*params.ny/LOCALSIZEY; i++){
      tot_u += tot_up[i+tt*params.nx/LOCALSIZEX*params.ny/LOCALSIZEY];
      tot_cells += tot_cellsp[i+tt*params.nx/LOCALSIZEX*params.ny/LOCALSIZEY];
      //printf("%d %f %d\n", i, tot_u, tot_cells);
    }
    av_vels[tt] = tot_u/tot_cells;
  }

  //end timer
  gettimeofday(&timstr, NULL);
  toc = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  // put answers back into cells
  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      cells[ii + jj*params.nx].speeds[0] = speedsHostS0[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[1] = speedsHostS1[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[2] = speedsHostS2[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[3] = speedsHostS3[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[4] = speedsHostS4[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[5] = speedsHostS5[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[6] = speedsHostS6[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[7] = speedsHostS7[ii + jj*params.nx];
      cells[ii + jj*params.nx].speeds[8] = speedsHostS8[ii + jj*params.nx];
    }
  }

  /* write final values and free memory */
  printf("==done==\n");
  printf("Reynolds number:\t\t%.12E\n", calc_reynolds(params, cells, obstaclesHost));
  printf("Elapsed time:\t\t\t%.6lf (s)\n", toc - tic);
  write_values(params, cells, obstaclesHost, av_vels);
  finalise(cells, tmp_cells, obstaclesHost, av_vels);

  free(speedsHostS0);
  free(speedsHostS1);
  free(speedsHostS2);
  free(speedsHostS3);
  free(speedsHostS4);
  free(speedsHostS5);
  free(speedsHostS6);
  free(speedsHostS7);
  free(speedsHostS8);
  free(tot_up);
  free(tot_cellsp);

  return EXIT_SUCCESS;
}


float av_velocity(const t_param params, t_speed* cells, int* obstacles)
{
  int    tot_cells = 0;  /* no. of cells used in calculation */
  float tot_u;          /* accumulated magnitudes of velocity for each cell */

  /* initialise */
  tot_u = 0.f;

  /* loop over all non-blocked cells */
  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* ignore occupied cells */
      if (!obstacles[ii + jj*params.nx])
      {
        /* local density total */
        float local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += cells[ii + jj*params.nx].speeds[kk];
        }

        /* x-component of velocity */
        float u_x = (cells[ii + jj*params.nx].speeds[1]
            + cells[ii + jj*params.nx].speeds[5]
            + cells[ii + jj*params.nx].speeds[8]
            - (cells[ii + jj*params.nx].speeds[3]
              + cells[ii + jj*params.nx].speeds[6]
              + cells[ii + jj*params.nx].speeds[7]))
          / local_density;
        /* compute y velocity component */
        float u_y = (cells[ii + jj*params.nx].speeds[2]
            + cells[ii + jj*params.nx].speeds[5]
            + cells[ii + jj*params.nx].speeds[6]
            - (cells[ii + jj*params.nx].speeds[4]
              + cells[ii + jj*params.nx].speeds[7]
              + cells[ii + jj*params.nx].speeds[8]))
          / local_density;
        /* accumulate the norm of x- and y- velocity components */
        tot_u += sqrtf((u_x * u_x) + (u_y * u_y));
        /* increase counter of inspected cells */
        ++tot_cells;
      }
    }
  }

  return tot_u / (float)tot_cells;
}

int initialise(const char* paramfile, const char* obstaclefile,
    t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
    int** obstacles_ptr, float** av_vels_ptr){
  char   message[1024];  /* message buffer */
  FILE*  fp;             /* file pointer */
  int    xx, yy;         /* generic array indices */
  int    blocked;        /* indicates whether a cell is blocked by an obstacle */
  int    retval;         /* to hold return value for checking */

  /* open the parameter file */
  fp = fopen(paramfile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input parameter file: %s", paramfile);
    die(message, __LINE__, __FILE__);
  }
  /* read in the parameter values */
  retval = fscanf(fp, "%d\n", &(params->nx));

  if (retval != 1) die("could not read param file: nx", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->ny));

  if (retval != 1) die("could not read param file: ny", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->maxIters));

  if (retval != 1) die("could not read param file: maxIters", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->reynolds_dim));

  if (retval != 1) die("could not read param file: reynolds_dim", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->density));

  if (retval != 1) die("could not read param file: density", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->accel));

  if (retval != 1) die("could not read param file: accel", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->omega));

  if (retval != 1) die("could not read param file: omega", __LINE__, __FILE__);

  /* and close up the file */
  fclose(fp);

  /*
   ** allocate memory.
   **
   ** remember C is pass-by-value, so we need to
   ** pass pointers into the initialise function.
   **
   ** nb we are allocating a 1D array, so that the
   ** memory will be contiguous.  We still want to
   ** index this memory as if it were a (row major
   ** ordered) 2D array, however.  We will perform
   ** some arithmetic using the row and column
   ** coordinates, inside the square brackets, when
   ** we want to access elements of this array.
   **
   ** note also that we are using a structure to
   ** hold an array of 'speeds'.  We will allocate
   ** a 1D array of these structs.
   */

  /* main grid */
  *cells_ptr = (t_speed*)malloc(sizeof(t_speed) * (params->ny * params->nx));

  if (*cells_ptr == NULL) die("cannot allocate memory for cells", __LINE__, __FILE__);

  /* 'helper' grid, used as scratch space */
  *tmp_cells_ptr = (t_speed*)malloc(sizeof(t_speed) * (params->ny * params->nx));

  if (*tmp_cells_ptr == NULL) die("cannot allocate memory for tmp_cells", __LINE__, __FILE__);

  /* the map of obstacles */
  *obstacles_ptr = (int*) malloc (sizeof(int) * params->ny * params->nx);

  if (*obstacles_ptr == NULL) die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  /* initialise densities */
  float w0 = params->density * 4.f / 9.f;
  float w1 = params->density      / 9.f;
  float w2 = params->density      / 36.f;

  for (int jj = 0; jj < params->ny; jj++)
  {
    for (int ii = 0; ii < params->nx; ii++)
    {
      /* centre */
      (*cells_ptr)[ii + jj*params->nx].speeds[0] = w0;
      /* axis directions */
      (*cells_ptr)[ii + jj*params->nx].speeds[1] = w1;
      (*cells_ptr)[ii + jj*params->nx].speeds[2] = w1;
      (*cells_ptr)[ii + jj*params->nx].speeds[3] = w1;
      (*cells_ptr)[ii + jj*params->nx].speeds[4] = w1;
      /* diagonals */
      (*cells_ptr)[ii + jj*params->nx].speeds[5] = w2;
      (*cells_ptr)[ii + jj*params->nx].speeds[6] = w2;
      (*cells_ptr)[ii + jj*params->nx].speeds[7] = w2;
      (*cells_ptr)[ii + jj*params->nx].speeds[8] = w2;
    }
  }

  /* first set all cells in obstacle array to zero */
  for (int jj = 0; jj < params->ny; jj++)
  {
    for (int ii = 0; ii < params->nx; ii++)
    {
      (*obstacles_ptr)[ii + jj*params->nx] = 0;
    }
  }

  /* open the obstacle data file */
  fp = fopen(obstaclefile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input obstacles file: %s", obstaclefile);
    die(message, __LINE__, __FILE__);
  }

  /* read-in the blocked cells list */
  while ((retval = fscanf(fp, "%d %d %d\n", &xx, &yy, &blocked)) != EOF)
  {
    /* some checks */
    if (retval != 3) die("expected 3 values per line in obstacle file", __LINE__, __FILE__);

    if (xx < 0 || xx > params->nx - 1) die("obstacle x-coord out of range", __LINE__, __FILE__);

    if (yy < 0 || yy > params->ny - 1) die("obstacle y-coord out of range", __LINE__, __FILE__);

    if (blocked != 1) die("obstacle blocked value should be 1", __LINE__, __FILE__);

    /* assign to array */
    (*obstacles_ptr)[xx + yy*params->nx] = blocked;
  }

  /* and close the file */
  fclose(fp);

  /*
   ** allocate space to hold a record of the avarage velocities computed
   ** at each timestep
   */
  *av_vels_ptr = (float*)malloc(sizeof(float) * params->maxIters);

  return EXIT_SUCCESS;
}

int finalise(t_speed* cells_ptr, t_speed* tmp_cells_ptr,
    int* obstacles_ptr, float* av_vels_ptr)
{
  /*
   ** free up allocated memory
   */
  free(cells_ptr);
  free(tmp_cells_ptr);
  free(obstacles_ptr);
  free(av_vels_ptr);

  return EXIT_SUCCESS;
}


float calc_reynolds(const t_param params, t_speed* cells, int* obstacles)
{
  const float viscosity = 1.f / 6.f * (2.f / params.omega - 1.f);

  return av_velocity(params, cells, obstacles) * params.reynolds_dim / viscosity;
}

float total_density(const t_param params, t_speed* cells)
{
  float total = 0.f;  /* accumulator */

  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      for (int kk = 0; kk < NSPEEDS; kk++)
      {
        total += cells[ii + jj*params.nx].speeds[kk];
      }
    }
  }

  return total;
}

int write_values(const t_param params, t_speed* cells, int* obstacles, float* av_vels)
{
  FILE* fp;                     /* file pointer */
  const float c_sq = 1.f / 3.f; /* sq. of speed of sound */
  float local_density;         /* per grid cell sum of densities */
  float pressure;              /* fluid pressure in grid cell */
  float u_x;                   /* x-component of velocity in grid cell */
  float u_y;                   /* y-component of velocity in grid cell */
  float u;                     /* norm--root of summed squares--of u_x and u_y */

  fp = fopen(FINALSTATEFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* an occupied cell */
      if (obstacles[ii + jj*params.nx])
      {
        u_x = u_y = u = 0.f;
        pressure = params.density * c_sq;
      }
      /* no obstacle */
      else
      {
        local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += cells[ii + jj*params.nx].speeds[kk];
        }

        /* compute x velocity component */
        u_x = (cells[ii + jj*params.nx].speeds[1]
            + cells[ii + jj*params.nx].speeds[5]
            + cells[ii + jj*params.nx].speeds[8]
            - (cells[ii + jj*params.nx].speeds[3]
              + cells[ii + jj*params.nx].speeds[6]
              + cells[ii + jj*params.nx].speeds[7]))
          / local_density;
        /* compute y velocity component */
        u_y = (cells[ii + jj*params.nx].speeds[2]
            + cells[ii + jj*params.nx].speeds[5]
            + cells[ii + jj*params.nx].speeds[6]
            - (cells[ii + jj*params.nx].speeds[4]
              + cells[ii + jj*params.nx].speeds[7]
              + cells[ii + jj*params.nx].speeds[8]))
          / local_density;
        /* compute norm of velocity */
        u = sqrtf((u_x * u_x) + (u_y * u_y));
        /* compute pressure */
        pressure = local_density * c_sq;
      }

      /* write to file */
      fprintf(fp, "%d %d %.12E %.12E %.12E %.12E %d\n", ii, jj, u_x, u_y, u, pressure, obstacles[ii * params.nx + jj]);
    }
  }

  fclose(fp);

  fp = fopen(AVVELSFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int ii = 0; ii < params.maxIters; ii++)
  {
    fprintf(fp, "%d:\t%.12E\n", ii, av_vels[ii]);
  }

  fclose(fp);

  return EXIT_SUCCESS;
}

void die(const char* message, const int line, const char* file)
{
  fprintf(stderr, "Error at line %d of file %s:\n", line, file);
  fprintf(stderr, "%s\n", message);
  fflush(stderr);
  exit(EXIT_FAILURE);
}

void usage(const char* exe)
{
  fprintf(stderr, "Usage: %s <paramfile> <obstaclefile>\n", exe);
  exit(EXIT_FAILURE);
}

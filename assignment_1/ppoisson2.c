/*
 * SEQ_Poisson.c
 * 2D Poison equation solver
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <mpi.h>

#define DEBUG 1

#define max(a, b) ((a) > (b) ? a : b)

enum
{
  X_DIR,
  Y_DIR
};

/* global variables */
int gridsize[2];
double precision_goal = 0.0001; /* precision_goal of solution */
int max_iter = 5000;            /* maximum number of iterations alowed */
MPI_Datatype border_type[2];    /* Datatypes for vertical and horizontal exchange */
int *gridsizes;
int grid_length;
int grid_size_idx = 0;

/* process specific variables */
int proc_rank;                                    /* process rank and number of ranks */
int proc_coord[2];                                /* coordinates of current process in processgrid */
int proc_top, proc_right, proc_bottom, proc_left; /* ranks of neigboring procs        */
int offset[2];                                    /* offset of subgrid handled by current process */

int P;              /* total number of processes */
int P_grid[2];      /* process grid dimensions        */
MPI_Comm grid_comm; /* grid COMMUNICATOR        */
MPI_Status status;

/* benchmark related variables */
clock_t ticks;    /* number of systemticks */
double *cpu_util; /* CPU utilization */
int timer_on = 0; /* is timer running? */
double wtime;
double *wtimes;
int count;
int *iters;

/* local grid related variables */
double **phi; /* grid */
int **source; /* TRUE if subgrid element is a source */
int dim[2];   /* grid dimensions */

/* relaxation paramater */
double omega;
double *omegas;
int omega_length;
int omega_optimal = 0;

/* function declarations */
void Setup_Grid();
void Setup_Proc_Grid(int argc, char **argv);
void Get_CLIs(int argc, char **argv);
void Setup_MPI_Datatypes();
void Exchange_Borders();
double Do_Step(int parity);
void Solve();
void Write_Grid();
void Benchmark();
void Clean_Up();
void Debug(char *mesg, int terminate);
void start_timer();
void resume_timer();
void stop_timer();
void print_timer();

void start_timer()
{
  if (!timer_on)
  {
    MPI_Barrier(MPI_COMM_WORLD);
    ticks = clock();
    wtime = MPI_Wtime();
    timer_on = 1;
  }
}

void resume_timer()
{
  if (!timer_on)
  {
    ticks = clock() - ticks;
    wtime = MPI_Wtime() - wtime;
    timer_on = 1;
  }
}

void stop_timer()
{
  if (timer_on)
  {
    ticks = clock() - ticks;
    wtime = MPI_Wtime() - wtime;
    timer_on = 0;
  }
}

void print_timer()
{
  if (timer_on)
  {
    stop_timer();
    printf("(%i) Elapsed Wtime %14.6f s (%5.1f%% CPU)\n",
           proc_rank, wtime, 100.0 * ticks * (1.0 / CLOCKS_PER_SEC) / wtime);
    resume_timer();
  }
  else
  {
    printf("(%i) Elapsed Wtime %14.6f s (%5.1f%% CPU)\n",
           proc_rank, wtime, 100.0 * ticks * (1.0 / CLOCKS_PER_SEC) / wtime);
  }
}

void Debug(char *mesg, int terminate)
{
  if (DEBUG || terminate)
    printf("%s\n", mesg);
  if (terminate)
    exit(1);
}

void Setup_Grid()
{
  int x, y, s;
  double source_x, source_y, source_val;
  int upper_offset[2];
  FILE *f;

  // Debug("Setup_Subgrid", 0);

  if (proc_rank == 0)
  {
    f = fopen("input.dat", "r");
    if (f == NULL)
      Debug("Error opening input.dat", 1);
    fscanf(f, "nx: %i\n", &gridsize[X_DIR]);
    fscanf(f, "ny: %i\n", &gridsize[Y_DIR]);
    fscanf(f, "precision goal: %lf\n", &precision_goal);
    fscanf(f, "max iterations: %i\n", &max_iter);
  }

  gridsize[X_DIR] = gridsizes[grid_size_idx];
  gridsize[Y_DIR] = gridsizes[grid_size_idx];

  // broadcast gridsize, precision_goal and max_iter to all processes
  MPI_Barrier(grid_comm);
  MPI_Bcast(&gridsize, 2, MPI_INT, 0, grid_comm);
  MPI_Bcast(&precision_goal, 1, MPI_DOUBLE, 0, grid_comm);
  MPI_Bcast(&max_iter, 1, MPI_INT, 0, grid_comm);
  MPI_Barrier(grid_comm);

  /* Calculate top  left  corner  coordinates  of  local  grid  */
  offset[X_DIR] = gridsize[X_DIR] * proc_coord[X_DIR] / P_grid[X_DIR];
  offset[Y_DIR] = gridsize[Y_DIR] * proc_coord[Y_DIR] / P_grid[Y_DIR];
  upper_offset[X_DIR] = gridsize[X_DIR] * (proc_coord[X_DIR] + 1) / P_grid[X_DIR];
  upper_offset[Y_DIR] = gridsize[Y_DIR] * (proc_coord[Y_DIR] + 1) / P_grid[Y_DIR];

  /* Calculate dimensions of  local  grid  */
  dim[Y_DIR] = upper_offset[Y_DIR] - offset[Y_DIR];
  dim[X_DIR] = upper_offset[X_DIR] - offset[X_DIR];

  /* Add space for rows/columns of neighboring grid */
  dim[Y_DIR] += 2;
  dim[X_DIR] += 2;

  /* allocate memory */
  if ((phi = malloc(dim[X_DIR] * sizeof(*phi))) == NULL)
    Debug("Setup_Subgrid : malloc(phi) failed", 1);
  if ((source = malloc(dim[X_DIR] * sizeof(*source))) == NULL)
    Debug("Setup_Subgrid : malloc(source) failed", 1);
  if ((phi[0] = malloc(dim[Y_DIR] * dim[X_DIR] * sizeof(**phi))) == NULL)
    Debug("Setup_Subgrid : malloc(*phi) failed", 1);
  if ((source[0] = malloc(dim[Y_DIR] * dim[X_DIR] * sizeof(**source))) == NULL)
    Debug("Setup_Subgrid : malloc(*source) failed", 1);
  for (x = 1; x < dim[X_DIR]; x++)
  {
    phi[x] = phi[0] + x * dim[Y_DIR];
    source[x] = source[0] + x * dim[Y_DIR];
  }

  /* set all values to '0' */
  for (x = 0; x < dim[X_DIR]; x++)
    for (y = 0; y < dim[Y_DIR]; y++)
    {
      phi[x][y] = 0.0;
      source[x][y] = 0;
    }

  /* put sources in field */
  MPI_Barrier(grid_comm);
  do
  {
    if (proc_rank == 0)
    {
      s = fscanf(f, "source: %lf %lf %lf\n", &source_x, &source_y, &source_val);
    }
    MPI_Bcast(&s, 1, MPI_INT, 0, grid_comm);
    if (s == 3)
    {
      MPI_Bcast(&source_x, 1, MPI_DOUBLE, 0, grid_comm);
      MPI_Bcast(&source_y, 1, MPI_DOUBLE, 0, grid_comm);
      MPI_Bcast(&source_val, 1, MPI_DOUBLE, 0, grid_comm);
      x = source_x * gridsize[X_DIR];
      y = source_y * gridsize[Y_DIR];
      x += 1;
      y += 1;
      x = x - offset[X_DIR];
      y = y - offset[Y_DIR];
      if (x > 0 && x < dim[X_DIR] - 1 && y > 0 && y < dim[Y_DIR] - 1)
      { /* indices in domain of this process */
        phi[x][y] = source_val;
        source[x][y] = 1;
      }
    }
  } while (s == 3);
  MPI_Barrier(grid_comm);

  if (proc_rank == 0)
  {
    fclose(f);
  }
}

void Setup_Proc_Grid(int argc, char **argv)
{
  int wrap_around[2];
  int reorder;
  // Debug("My_MPI_Init", 0);

  /* Retrieve the number of processes */
  MPI_Comm_size(MPI_COMM_WORLD, &P); /* find out how many processes there are        */

  /* Calculate the number of processes per column and per row for the grid */
  if (argc > 2)
  {
    P_grid[X_DIR] = atoi(argv[1]);
    P_grid[Y_DIR] = atoi(argv[2]);
    if (P_grid[X_DIR] * P_grid[Y_DIR] != P)
      Debug("ERROR Proces grid dimensions do not match with P ", 1);
  }
  else
  {
    Debug("ERROR Wrong parameter input", 1);
  }

  /* Create process topology (2D grid) */
  wrap_around[X_DIR] = 0;
  wrap_around[Y_DIR] = 0; /*  do  not  connect  first  and last process        */

  reorder = 1; /*  reorder process ranks        */

  /* Creates a new communicator grid_comm  */
  MPI_Cart_create(MPI_COMM_WORLD, 2, P_grid, wrap_around, reorder, &grid_comm);

  /* Retrieve new rank and cartesian coordinates of this process */
  MPI_Comm_rank(grid_comm, &proc_rank);                 /*  Rank  of  process  in  new  communicator        */
  MPI_Cart_coords(grid_comm, proc_rank, 2, proc_coord); /* Coordinates of process in new communicator */

  printf("(%i) (x,y)=(%i,%i)\n", proc_rank, proc_coord[X_DIR], proc_coord[Y_DIR]);

  /* calculate ranks of neighboring processes */
  MPI_Cart_shift(grid_comm, X_DIR, 1, &proc_left, &proc_right);

  /*  rank of processes proc_top and proc_bottom  */
  MPI_Cart_shift(grid_comm, Y_DIR, -1, &proc_bottom, &proc_top);

  if (DEBUG)
    printf("(%i) top %i,  right  %i,  bottom  %i,  left  %i\n", proc_rank, proc_top,
           proc_right, proc_bottom, proc_left);
}

void Get_CLIs(int argc, char **argv)
{
  int l, i;
  l = 0;
  double omega_start, omega_end, omega_step;
  int grid_start, grid_end, grid_step;
  if (argc > 3)
  {
    while (l < argc)
    {
      if (strcmp(argv[l], "-omega") == 0)
      {
        printf("(%i) Using omega value from command line\n", proc_rank);
        omegas = malloc(sizeof(double));
        omegas[0] = atof(argv[4]);
        omega_length = 1;
      }

      if (strcmp(argv[l], "-omegas") == 0)
      {
        printf("(%i) Using omega values from command line\n", proc_rank);
        omega_optimal = 1;
        omega_start = atof(argv[l + 1]);
        omega_end = atof(argv[l + 2]);
        omega_step = atof(argv[l + 3]);
        omega_length = (int)((omega_end - omega_start) / omega_step) + 1;
        if (omega_start < 0.0 || omega_start > 2.0 || omega_end < 0.0 || omega_end > 2.0 || omega_step < 0.0 || omega_step > 2.0)
          Debug("ERROR Omega values outside range [0,2]", 1);
        omegas = malloc(omega_length * sizeof(double));
        for (i = 0; i < omega_length; i++)
        {
          omegas[i] = omega_start + (double)i * omega_step;
        }
      }

      if (strcmp(argv[l], "-grid") == 0)
      {
        printf("(%i) Using grid size from command line\n", proc_rank);
        gridsizes = malloc(sizeof(int));
        gridsizes[0] = atoi(argv[l + 1]);
        grid_length = 1;
      }

      if (strcmp(argv[l], "-grids") == 0)
      {
        printf("(%i) Using grid sizes from command line\n", proc_rank);
        grid_start = atoi(argv[l + 1]);
        grid_end = atoi(argv[l + 2]);
        grid_step = atoi(argv[l + 3]);
        grid_length = (int)((grid_end - grid_start) / grid_step) + 1;
        if (grid_start < 0 || grid_end < 0 || grid_step < 0)
          Debug("ERROR Grid values outside range [0,inf]", 1);
        gridsizes = malloc(grid_length * sizeof(int));
        for (i = 0; i < grid_length; i++)
        {
          gridsizes[i] = grid_start + i * grid_step;
        }
      }

      l++;
    }
  }
  else
  {
    printf("(%i) No CLI args specified, using default values for grid size and omega\n", proc_rank);
    omegas = malloc(sizeof(double));
    omegas[0] = 1.95;
    omega_length = 1;
    gridsizes = malloc(sizeof(int));
    gridsizes[0] = 100;
    grid_length = 1;
  }
}

double Do_Step(int parity)
{
  int x, y;
  double old_phi;
  double max_err = 0.0;

  /* calculate interior of grid */
  for (x = 1; x < dim[X_DIR] - 1; x++)
    for (y = 1; y < dim[Y_DIR] - 1; y++)
      if ((offset[X_DIR] + x + offset[Y_DIR] + y) % 2 == parity && source[x][y] != 1)
      {
        old_phi = phi[x][y];
        phi[x][y] = (1 - omega) * phi[x][y] + omega * (phi[x + 1][y] + phi[x - 1][y] + phi[x][y + 1] + phi[x][y - 1]) * 0.25;
        if (max_err < fabs(old_phi - phi[x][y]))
          max_err = fabs(old_phi - phi[x][y]);
      }

  return max_err;
}

void Solve()
{
  count = 0;
  double delta;
  double global_delta;
  double delta1, delta2;

  // Debug("Solve", 0);

  /* give global_delta a higher value then precision_goal */
  global_delta = 2 * precision_goal;
  while (global_delta > precision_goal && count < max_iter)
  {
    // Debug("Do_Step 0", 0);
    delta1 = Do_Step(0);
    Exchange_Borders();

    // Debug("Do_Step 1", 0);
    delta2 = Do_Step(1);
    Exchange_Borders();

    delta = max(delta1, delta2);
    MPI_Allreduce(&delta, &global_delta, 1, MPI_DOUBLE, MPI_MAX, grid_comm);

    count++;
  }
  printf("(%i) Gridsize: %i,  Omega: %.2f, Iterations: %i, Error: %.2e\n", proc_rank, gridsize[X_DIR], omega, count, global_delta);
}

void Write_Grid()
{
  int x, y;
  FILE *f;

  if (proc_rank == 0)
  {
    char fn_template[] = "output/ppoisson2_gs=%ix%i_nproc=%i_omega=%.2f.dat";
    char fn[100];
    sprintf(fn, fn_template, gridsize[X_DIR], gridsize[Y_DIR], P, omega);
    if ((f = fopen(fn, "w")) == NULL)
      Debug("Write_Grid : fopen failed", 1);

    // gather all data from other processes
    for (int i = 0; i < P; i++)
    {
      if (i > 0)
      {
        MPI_Recv(&phi[0][0], dim[X_DIR] * dim[Y_DIR], MPI_DOUBLE, i, 0, grid_comm, &status);
        MPI_Recv(&offset, 2, MPI_INT, i, 0, grid_comm, &status);
      }
      for (x = 1; x < dim[X_DIR] - 1; x++)
        for (y = 1; y < dim[Y_DIR] - 1; y++)
          fprintf(f, "%i %i %f\n", offset[X_DIR] + x, offset[Y_DIR] + y, phi[x][y]);
    }
    fclose(f);
  }
  else
  {
    MPI_Send(&phi[0][0], dim[X_DIR] * dim[Y_DIR], MPI_DOUBLE, 0, 0, grid_comm);
    MPI_Send(&offset, 2, MPI_INT, 0, 0, grid_comm);
  }
}

void Benchmark()
{
  double ***benchmark; /* 3D array holding benchmark results shape 2 x #processors x #omegas*/
  int benchmark_size;
  // Debug("Benchmark", 0);

  // save time, cpu_util, omega in root process
  if (proc_rank == 0)
  {
    benchmark_size = 2 * P * omega_length;
    benchmark = malloc(2 * sizeof(double **) * P);
    for (int i = 0; i < 2 * P; i++)
    {
      benchmark[i] = malloc(P * sizeof(double *) * omega_length);
      for (int j = 0; j < P; j++)
      {
        benchmark[i][j] = malloc(omega_length * sizeof(double) * 2);
      }
    }
    for (int j = 0; j < omega_length; j++)
    {
      benchmark[0][0][j] = wtimes[j];
      benchmark[1][0][j] = cpu_util[j];
    }
  }

  // gather times
  if (proc_rank == 0)
  {
    for (int i = 1; i < P; i++)
    {
      MPI_Recv(&benchmark[0][i][0], omega_length, MPI_DOUBLE, i, 0, grid_comm, &status);
    }
  }
  else
  {
    MPI_Send(wtimes, omega_length, MPI_DOUBLE, 0, 0, grid_comm);
  }

  MPI_Barrier(grid_comm);

  // gather cpu util
  if (proc_rank == 0)
  {
    for (int i = 1; i < P; i++)
    {
      MPI_Recv(&benchmark[1][i][0], omega_length, MPI_DOUBLE, i, 1, grid_comm, &status);
    }
  }
  else
  {
    MPI_Send(cpu_util, omega_length, MPI_DOUBLE, 0, 1, grid_comm);
  }

  MPI_Barrier(grid_comm);

  if (proc_rank == 0)
  {
    char fn_template[] = "ppoisson_times/ppoisson2_gs=%ix%i_nproc=%i_wl=%3.2f_wh=%3.2f_nomega=%i_times.dat";
    char fn[200];
    sprintf(fn, fn_template, gridsize[X_DIR], gridsize[Y_DIR], P, omegas[0], omegas[omega_length - 1], omega_length);
    FILE *f = fopen(fn, "w");
    if (f == NULL)
      Debug("Error opening benchmark file", 1);

    if (fwrite(benchmark, sizeof(double), benchmark_size, f) != benchmark_size)
    {
      Debug("File write error.", 1);
      exit(1);
    }
    fclose(f);

    //  save omega values to file
    char fn_template2[] = "ppoisson_times/ppoisson2_gs=%ix%i_nproc=%i_wl=%3.2f_wh=%3.2f_nomega=%i_omegas.dat";
    char fn2[200];
    sprintf(fn2, fn_template2, gridsize[X_DIR], gridsize[Y_DIR], P, omegas[0], omegas[omega_length - 1], omega_length);
    FILE *f2 = fopen(fn2, "w");
    if (f2 == NULL)
      Debug("Error opening benchmark file", 1);

    if (fwrite(omegas, sizeof(double), omega_length, f2) != omega_length)
    {
      Debug("File write error.", 1);
      exit(1);
    }

    fclose(f2);

    //  save iters to file
    char fn_template3[] = "ppoisson_times/ppoisson2_gs=%ix%i_nproc=%i_wl=%3.2f_wh=%3.2f_nomega=%i_iters.dat";
    char fn3[200];
    sprintf(fn3, fn_template3, gridsize[X_DIR], gridsize[Y_DIR], P, omegas[0], omegas[omega_length - 1], omega_length);
    FILE *f3 = fopen(fn3, "w");
    if (f3 == NULL)
      Debug("Error opening benchmark file", 1);

    if (fwrite(iters, sizeof(double), omega_length, f3) != omega_length)
    {
      Debug("File write error.", 1);
      exit(1);
    }

    fclose(f3);
  }
}

void Clean_Up()
{
  // Debug("Clean_Up", 0);

  free(phi[0]);
  free(phi);
  free(source[0]);
  free(source);
}

void Setup_MPI_Datatypes()
{
  // Debug("Setup_MPI_Datatypes", 0);

  /* Datatype for vertical data exchange (Y_DIR) */
  MPI_Type_vector(dim[X_DIR] - 2, 1, dim[Y_DIR],
                  MPI_DOUBLE, &border_type[Y_DIR]);
  MPI_Type_commit(&border_type[Y_DIR]);

  /* Datatype for horizontal data exchange (X_DIR) */
  MPI_Type_vector(dim[Y_DIR] - 2, 1, 1,
                  MPI_DOUBLE, &border_type[X_DIR]);
  MPI_Type_commit(&border_type[X_DIR]);
}

void Exchange_Borders()
{
  //   // Debug("Exchange_Borders", 0);
  MPI_Sendrecv(&phi[1][1], 1, border_type[Y_DIR], proc_top, 0,
               &phi[1][dim[Y_DIR] - 1], 1, border_type[Y_DIR], proc_bottom, 0, grid_comm, &status); /*  all  traffic in direction "top"        */
  MPI_Sendrecv(&phi[1][dim[Y_DIR] - 2], 1, border_type[Y_DIR], proc_bottom, 0,
               &phi[1][0], 1, border_type[Y_DIR], proc_top, 0, grid_comm, &status); /*  all  traffic in direction "bottom"        */
  MPI_Sendrecv(&phi[1][1], 1, border_type[X_DIR], proc_left, 0,
               &phi[dim[X_DIR] - 1][1], 1, border_type[X_DIR], proc_right, 0, grid_comm, &status); /* all traffic in direction "left" */
  MPI_Sendrecv(&phi[dim[X_DIR] - 2][1], 1, border_type[X_DIR], proc_right, 0,
               &phi[0][1], 1, border_type[X_DIR], proc_left, 0, grid_comm, &status); /* all traffic in the direction "right" */
}

int main(int argc, char **argv)
{
  MPI_Init(&argc, &argv);

  Setup_Proc_Grid(argc, argv);

  Get_CLIs(argc, argv);

  iters = malloc(omega_length * sizeof(int));
  wtimes = malloc(omega_length * sizeof(double));
  cpu_util = malloc(omega_length * sizeof(double));

  for (grid_size_idx; grid_size_idx < grid_length; grid_size_idx++)
  {
    // gridsize[X_DIR] = gridsizes[g];
    // gridsize[Y_DIR] = gridsizes[g];

    // MPI_Barrier(grid_comm);
    // MPI_Bcast(&gridsize, 2, MPI_INT, 0, grid_comm);
    // MPI_Bcast(&precision_goal, 1, MPI_DOUBLE, 0, grid_comm);
    // MPI_Bcast(&max_iter, 1, MPI_INT, 0, grid_comm);
    // MPI_Barrier(grid_comm);

    for (int i = 0; i < omega_length; i++)
    {
      omega = omegas[i];

      Setup_Grid();

      Setup_MPI_Datatypes();

      start_timer();

      Solve();

      stop_timer();

      // print_timer();

      if (omega_optimal == 0) // only write grid if we are not testing omega optimality
      {
        Write_Grid();
      }

      // benchmarking
      iters[i] = count;
      wtimes[i] = wtime;
      cpu_util[i] = 100.0 * ticks * (1.0 / CLOCKS_PER_SEC) / wtime;

      MPI_Barrier(grid_comm);
      
      Clean_Up();
    }

    Benchmark();
  }

  MPI_Finalize();

  return 0;
}

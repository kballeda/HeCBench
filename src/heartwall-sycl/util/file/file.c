#include <stdio.h>                    // needed by types/functions related to file handling
#include "./../../main.h"                // needed for definition of fp
#include "./file.h"

void 
read_parameters(const char* filename, int* tSize, int* sSize, int* maxMove, FP* alpha){

  //================================================================================80
  //  VARIABLES
  //================================================================================80

  FILE* fid;

  //================================================================================80
  //  OPEN FILE FOR READING
  //================================================================================80

  fid = fopen(filename, "r");
  if( fid == NULL ){
    printf( "The file was not opened for reading\n" );
    return;
  }

  //================================================================================80
  //  READ VALUES FROM THE FILE
  //================================================================================80

  fscanf(fid, "%d", &tSize[0]);
  fscanf(fid, "%d", &sSize[0]);
  fscanf(fid, "%d", &maxMove[0]);
  fscanf(fid, "%f", &alpha[0]);

  fclose(fid);
}

void 
read_header(const char* filename, int* size, int* size_2){

  //================================================================================80
  //  VARIABLES
  //================================================================================80

  FILE* fid;
  int i;
  char c;

  //================================================================================80
  //  OPEN FILE FOR READING
  //================================================================================80

  fid = fopen(filename, "r");
  if( fid == NULL ){
    printf( "The file was not opened for reading\n" );
    return;
  }

  //================================================================================80
  //  SKIP LINES
  //================================================================================80

  i = 0;
  while(i<1){
    c = fgetc(fid);
    if(c == '\n'){
      i = i+1;
    }
  };

  //================================================================================80
  //  READ VALUES FROM THE FILE
  //================================================================================80

  fscanf(fid, "%d", &size[0]);

  fscanf(fid, "%d", &size_2[0]);

  fclose(fid);
}

void 
read_data(
    const char* filename,
    int size,
    int* input_a,
    int* input_b,
    int size_2,
    int* input_2a,
    int* input_2b){

  //================================================================================80
  //  VARIABLES
  //================================================================================80

  FILE* fid;
  int i;
  char c;

  //================================================================================80
  //  OPEN FILE FOR READING
  //================================================================================80

  fid = fopen(filename, "r");
  if( fid == NULL ){
    printf( "The file was not opened for reading\n" );
    return;
  }

  //================================================================================80
  //  SKIP LINES
  //================================================================================80

  i = 0;
  while(i<2){
    c = fgetc(fid);
    if(c == '\n'){
      i = i+1;
    }
  };

  //================================================================================80
  //  READ VALUES FROM THE FILE
  //================================================================================80

  for(i=0; i<size; i++){
    fscanf(fid, "%d", &input_a[i]);
  }
  for(i=0; i<size; i++){
    fscanf(fid, "%d", &input_b[i]);
  }

  for(i=0; i<size_2; i++){
    fscanf(fid, "%d", &input_2a[i]);
  }
  for(i=0; i<size_2; i++){
    fscanf(fid, "%d", &input_2b[i]);
  }

  fclose(fid);
}

void
write_data(
    const char* filename,
    int frameNo,
    int frames_processed,
    int endoPoints,
    int* input_a,
    int* input_b,
    int epiPoints,
    int* input_2a,
    int* input_2b){

  //================================================================================80
  //  VARIABLES
  //================================================================================80

  FILE* fid;
  int i,j;

  //================================================================================80
  //  OPEN FILE FOR READING
  //================================================================================80

  fid = fopen(filename, "w+");
  if( fid == NULL ){
    printf( "The file was not opened for writing\n" );
    return;
  }

  //================================================================================80
  //  WRITE VALUES TO THE FILE
  //================================================================================80
  fprintf(fid, "Total AVI Frames: %d\n", frameNo);  
  fprintf(fid, "Frames Processed: %d\n", frames_processed);  
  fprintf(fid, "endoPoints: %d\n", endoPoints);
  fprintf(fid, "epiPoints: %d", epiPoints);
  for(j=0; j<frames_processed;j++)
  {
    fprintf(fid, "\n---Frame %d---",j);
    fprintf(fid, "\n--endo--\n");
    for(i=0; i<endoPoints; i++){
      fprintf(fid, "%d\t", input_a[j+i*frameNo]);
    }
    fprintf(fid, "\n");
    for(i=0; i<endoPoints; i++){
      // if(input_b[j*size+i] > 2000) input_b[j*size+i]=0;
      fprintf(fid, "%d\t", input_b[j+i*frameNo]);
    }
    fprintf(fid, "\n--epi--\n");
    for(i=0; i<epiPoints; i++){
      //if(input_2a[j*size_2+i] > 2000) input_2a[j*size_2+i]=0;
      fprintf(fid, "%d\t", input_2a[j+i*frameNo]);
    }
    fprintf(fid, "\n");
    for(i=0; i<epiPoints; i++){
      //if(input_2b[j*size_2+i] > 2000) input_2b[j*size_2+i]=0;
      fprintf(fid, "%d\t", input_2b[j+i*frameNo]);
    }
  }

  fclose(fid);
}

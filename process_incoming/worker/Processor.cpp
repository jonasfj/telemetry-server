#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <string>
#include <fstream>

#include "RecordWriter.h"

#define MAX_SIZE    1024 * 1024 * 1024
#define MAX_MEM     512 * 1024 * 1024

using namespace std;
using namespace mozilla::Telemetry;

/** Main function */
int main(int argc, const char* argv[])
{
  string inputFile = "raw-unsorted-data.log";
  string workingFolder = "working/";
  string uploadFolder = "upload/";

  RecordWriter writer(workingFolder, uploadFolder, MAX_SIZE, MAX_MEM, 0);

  ifstream input(inputFile);

  int i = 0;

  // Read input line by line
  string line;
  while(getline(input, line)) {

    // Find first space, this is where we split the file
    size_t split = line.find_first_of(' ');
    if (split == string::npos) {
      // No space, split... must be invalid line
      fprintf(stderr, "Line doesn't contain a space!\n");
      continue;
    }

    // Extra path
    string path = line.substr(0, split);
  
    // Find second space
    split = line.find_first_of(' ', split + 1);
    const char* buffer = line.c_str() + split + 1;

    writer.Write(path, buffer, strlen(buffer));

    i++;
  }

  writer.Finalize();

  fprintf(stderr, "Finish writing %i records\n", i);
  return 0;
}




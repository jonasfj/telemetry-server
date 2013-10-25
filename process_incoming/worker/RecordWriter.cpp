#include "CompressedFileWriter.h"
#include "Utils.h"

#include "RecordWriter.h"

#include <errno.h>
#include <string.h>
#include <assert.h>

#include <vector>
#include <algorithm>

using namespace std;

namespace mu = mozilla::Utils;

/** Compression memory requirements from man xz(1) */
static size_t PresetCompressionContextMemorySize[] = {
  3145728, 9437184, 17825792, 33554432, 50331648, 98566144, 98566144,
  195035136, 387973120, 706740224
};

/** 
 * Minimum number of bytes initiating on-the-fly compression, compression
 * overhead of compressing everything 20 MiB chunks is negligible. Thus, when
 * when we first start on-the-fly compression at 20 MiB we can always stop
 * on-the-fly compression again, this is useful of another OutputFile
 * receives more traffic.
 */
#define MIN_COMRESS_CHUNCK          20 * 1024 * 1024

namespace mozilla {
namespace Telemetry {

/** Context that tracks and assists with intermediate file output */
class RecordWriter::OutputFile
{
public:
  OutputFile(const string& aPath, RecordWriter& aOwner)
   : mOwner(aOwner), mCompressor(nullptr), mRecordsSinceLastReprioritization(0),
     mRawFile(NULL), mCompressedFile(NULL), mUncompressedRawSize(0),
     mUncompressedSize(0), mPath(aPath), mIsCorrupted(false)
  {
    // Initialized with no raw file or on-the-fly compression
  }

  /** Write a record to file */
  bool Write(const char* aRecord, size_t aLength);

  /** True, if on-the-fly compression can be added */
  bool CanAddCompression const
  {
    assert(!IsCorrupted());
    return mUncompressedRawSize > MIN_COMRESS_CHUNCK;
  }

  /** Compress existing file and keep on-the-fly compressor */
  bool AddCompressor();

  /** Remove associated compressor */
  bool RemoveCompressor();

  /** True, if this OutputFile has an on-the-fly compressor associated */
  bool HasCompressor()
  {
    assert(!IsCorrupted());
    return mCompressor != nullptr;
  }

  /**
   * True, if this file has experienced fatal errors, and file must be assumed
   * corrupted. If true, don't try to finalize this file, just finalize other
   * files and exit non-zero.
   */
  bool IsCorrupted() const
  {
    return mIsCorrupted;
  }

  /** Compress file, move to mUploadFolder */
  bool Finalize();

  /** Return filter path for this OutputFile */
  const string& Path() const
  {
    return mPath;
  }

  /** Get number of records written since last reprioritization */
  const uint32_t RecordsSinceLastReprioritization() const
  {
    assert(!IsCorrupted());
    return mRecordsSinceLastReprioritization;
  }

  /** Reset counter of records written since last reprioritization */
  void ResetReprioritizationRecordCounter()
  {
    assert(!IsCorrupted());
    mRecordsSinceLastReprioritization = 0;
  }

private:
  /** Record writer that owns this OutputFile */
  RecordWriter& mOwner;
  
  /** Compression context, if on-the-fly compression is active */
  CompressedFileWriter* mCompressor;

  /** Number of records written since last reprioritization */
  uint32_t  mRecordsSinceLastReprioritization;
  
  /** File handle for raw file, if not currently compressing */
  FILE* mRawFile;
  
  /** File handle for compressed file */
  FILE* mCompressedFile;
  
  /** Size of data current written to mRawFile */
  uint64_t mUncompressedRawSize;

  /** Size of data both in mRawFile and chunks compressed */
  uint64_t mUncompressedSize;

  /** Filter path string */
  string mPath;

  /** True, if this file is corrupted, ie. failures with undefined outcome */
  bool mIsCorrupted;

  /** Get working folder for this OutputFolder */
  string WorkFolder() const
  {
    return mOwner.WorkFolder() + mPath;
  }

  /** Get path to raw file */
  string RawPath() const
  {
    return WorkFolder() + "/raw-data.log";
  }

  /** Get path to compressed file */
  string CompressedPath() const
  {
    return WorkFolder() + "/data.log.xz";
  }

  /** Get folder to finished file for upload */
  string UploadFolder() const
  {
    return mOwner.UploadFolder() + mPath;
  }

  /** Get path to the finish file for upload */
  string FinishedPath() const
  {
    //<build-id>.<submission-date>.v<version>.log.<number of records>.<uuid4>.lzma
    return UploadFolder() + "/data.log.xz"; // +  mOwner.GetUUID()
  }
};


bool OutputFile::Write(const char* aRecord, size_t aLength)
{
  // Sanity checking internal state
  assert(!IsCorrupted());
  assert(!(mRawFile && mCompressor));
  assert(mUncompressedRawSize <= mUncompressedSize);

  // Count records written since last time things were re-prioritized
  mRecordsSinceLastReprioritization++;

  // Add to uncompressed size
  mUncompressedSize += aLength;

  // If we have compressor, write to it
  if (mCompressor) {
    assert(mCompressedFile && mUncompressedRawSize == 0);
    // If writing to compressed file fails, we return for clean process abortion
    if (!mCompressor.Write(aRecord, aLength)) {
      mIsCorrupted = true;
      return false;
    }
  } else {
    
    // If raw file is missing we open one
    if (!mRawFile) {
      mu::EnsurePath(WorkFolder());
      mRawFile = fopen(RawPath().c_str(), "w+");
      if (mRawFile == NULL) {
        fprintf(stderr, "OutputFile::Write: fopen failed: %s\n",
                        strerror(errno));
        return false;
      }
    }

    // Write to raw-file
    if (fwrite(aRecord, 1, aLength, mRawFile) != aLength) {
      fprintf(stderr, "OutputFile::Write: fwrite failed: %s\n",
                      strerror(errno));
      mIsCorrupted = true;
      return false;
    }

    // Remember what we wrote to mRawFile
    mUncompressedRawSize += aLength;
  }

  // Check if we need to rotate
  if (mUncompressedSize > mOwner.MaxUncompressedSize()) {
    // Finalize if uncompressed file size limit is reached
    return Finalize();
  }

  return true;
}

bool OutputFile::AddCompressor()
{
  // Sanity checking internal state
  assert(!IsCorrupted());
  assert(mRawFile && !mCompressor);
  assert(mUncompressedRawSize <= mUncompressedSize);

  // We don't have a compressed file, create one
  if (!mCompressedFile) {
    mCompressedFile = fopen(CompressedPath().c_str(), "a");
    if (mCompressedFile == NULL) {
      fprintf(stderr, "OutputFile::AddCompressor: fopen failed: %s\n",
                      strerror(errno));
      return false;
    }
  }

  // Create compressor
  mCompressor = new CompressedFileWriter(mCompressedFile);
  if (!mCompressor.Initialize(mOwner.CompressionPreset())) {
    mIsCorrupted = true;
    return false;
  }

  // Move to beginning of raw file
  rewind(mRawFile);

  // Create a buffer for reading file
  char* buffer = new char[BUFSIZ];

  // While file isn't at end
  while (!feof(mRawFile)) {
    size_t read = fread(buffer, 1, BUFSIZ, mRawFile);

    // Check for errors
    if (read < BUFSIZ && ferror(mRawFile)) {
      fprintf(stderr, "OutputFile::AddCompressor: ferror() return error!\n");
      mIsCorrupted = true;
      return false;
    }

    // Write to compressor
    if (!mCompressor.Write(buffer, read)) {
      mIsCorrupted = true;
      return false;
    }
  }

  // Close raw file
  if (fclose(mRawFile)) {
    fprintf(stderr, "OutputFile::AddCompressor: failed to fclose() raw file: "
                    "%s\n", strerror(errno));
    mIsCorrupted = true;
    return false;
  }
  mRawFile = NULL;

  // Remove raw file
  if (remove(RawPath().c_str())) {
    fprintf(stderr, "OutputFile::AddCompressor: failed to remove() raw file: "
                    "%s\n", strerror(errno));
    // This error is not fatal
  }

  return true;
}

bool OutputFile::RemoveCompressor()
{
  assert(!IsCorrupted());
  assert(!mRawFile);
  assert(mCompressor && mCompressedFile);

  // Finalize compressor
  if (mCompressor.Finalize()) {
    fprintf(stderr, "OutputFile::RemoveCompressor: Failed to finalize"
                    "compressor.\n");
    mIsCorrupted = true;
    return false;
  }

  // Delete compressor
  delete mCompressor;
  mCompressor = nullptr;
}


bool OutputFile::Finalize()
{
  assert(!IsCorrupted());

  // Check if there is anything to finalize
  if (!mRawFile && !HasCompressor() && !mCompressedFile) {
    return true;
  }

  // If we have raw file, add a compressor which compresses the file
  if (mRawFile) {
    assert(!HasCompressor());
    if (!AddCompressor()) {
      mIsCorrupted = true;
      return false;
    }
  }

  // If we have a compressor remove it
  if (HasCompressor()) {
    if (!RemoveCompressor()) {
      mIsCorrupted = true;
      return false;
    }
  }

  // Close the compressed file
  if (fclose(mCompressedFile)) {
    fprintf(stderr, "OutputFile::Finalize: failed to fclose() compressed file: "
                    "%s\n", strerror(errno));
    mIsCorrupted = true;
    return false;
  }
  mCompressedFile = NULL;

  // Create upload folder, so we can move the file there
  if (!mu::EnsurePath(UploadFolder())) {
    mIsCorrupted = true;
    return false;
  }

  // Move atomically to the upload folder
  if (frename(CompressedPath().c_str(), FinishedPath().c_str())) {
    fprintf(stderr, "OutputFile::AddCompressor: failed to rename() finished "
                    "file to upload folder: %s\n", strerror(errno));
    mIsCorrupted = true;
    return false;
  }

  return true;
}


RecordWriter::RecordWriter(const std::string& aWorkFolder,
                           const std::string& aUploadFolder,
                           uint64_t aMaxUncompressedSize,
                           size_t aSoftMemoryLimit, uint32_t aCompressionPreset)
   : mWorkFolder(aWorkFolder), mUploadFolder(aUploadFolder),
     mMaxUncompressedSize(aMaxUncompressedSize),
     mSoftMemoryLimit(aSoftMemoryLimit), mCompressionPreset(aCompressionPreset),
     mFileMap(), mRecordsSinceLastReprioritization(0)
  {
    // We don't allow use of extreme flag
    assert(mCompressionPreset <= 9);

    // Ensure that folder strings always end with a slash
    if (mWorkFolder[mWorkFolder.length() - 1] != '/') {
      mWorkFolder += "/";
    }
    if (mUploadFolder[mUploadFolder.length() - 1] != '/') {
      mUploadFolder += "/";
    }
  }


bool RecordWriter::Write(const string& aPath, const char* aRecord,
                         size_t aLength)
{
  // We've hit the repriorization interval reprioritize on-the-fly compression
  if (mRecordsSinceLastReprioritization++ > REPRIORIZATION_INTERVAL) {
    mRecordsSinceLastReprioritization = 0;
    if (!ReprioritizeCompression()) {
      return false;
    }
  }

  // OutputFile for this path
  OutputFile* file = nullptr;

  // Lookup in hash table
  auto it = mFileMap.find(aPath);
  if (it != mFileMap.end()) {
    file = it->second;
  } else {
    // Create output file
    file = new OutputFile(aPath, *this);
    // Store file for use next time
    mFileMap.insert({{aPath, file}});
  }
  assert(file);

  // Write record to file
  return file->Write(aRecord, aLength);
}

bool RecordWriter::Finalize()
{
  bool retval = true;

  // Serialize each file
  for (auto it = mFileMap.begin(); it != mFileMap.end(); it++) {
    const string& path = it->first;
    auto file = it->second;

    // Don't try to finalized corrupted files, behavior is undefined
    if (file->IsCorrupted()) {
      retval = false;
    } else {
      // Finalize file, compress and move to upload folder
      retval = file->Finalize() && retval;
    }

    delete file;
  }

  // Release map
  mFileMap.clear();

  return retval;
}

bool RecordWriter::ReprioritizeCompression()
{
  // Create a list of files that can be compressed
  vector<OutputFile*> files;
  for(auto kv : mFileMap) {
    if (kv.second->CanAddCompression()) {
      files.push_back(kv.second);
    }
  }

  // Sort after mRecordsSinceLastReprioritization
  sort(files.begin(), files.end(), [](const OutputFile& f1,
                                      const OutputFile& f2) {
    return f2.RecordsSinceLastReprioritization() <
           f1.RecordsSinceLastReprioritization();
  });

  // Figure out how many compression contexts we can have
  size_t context_size = PresetCompressionContextMemorySize[mCompressionPreset];
  int contexts = (mSoftMemoryLimit / context_size) - 1;
  if (contexts < 0)
    contexts = 0;

  // Remove compression from bottom most files
  int len = files.size();
  int i;
  for(i = contexts; i < len; i++) {
    if (files[i]->HasCompressor()) {
      if (!files[i]->RemoveCompressor()) {
        return false;
      }
    }
  }

  // Add compression to top most files
  for(i = 0; i < contexts; i++) {
    if (!files[i]->HasCompressor()) {
      if (!files[i]->AddCompressor()) {
        return false;
      }
    }
  }

  // Reset mRecordsSinceLastReprioritization
  for(auto kv : mFileMap) {
    kv.second->ResetReprioritizationRecordCounter();
  }
}


} // namespace Telemetry
} // namespace mozilla
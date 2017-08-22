#include "file_format.h"

namespace mip {
namespace file {

FileFormat::FileFormat(
    std::shared_ptr<IStream> file,
    const string& extension)
  : mFile(file),
    mExtension(extension),
    mTags(),
    mHasTags(false) {
}

const vector<mip::Tag>& mip::file::FileFormat::GetTags() {
  if (!mHasTags) {
    mTags = ReadTags();
    mHasTags = true;
  }

  return mTags;
}

void mip::file::FileFormat::SetTags(const vector<mip::Tag>&tags) {
  mTags = tags;
  mHasTags = true;
}

} //namespace file
} //namespace mip

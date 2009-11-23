// __BEGIN_LICENSE__
// Copyright (C) 2006-2009 United States Government as represented by
// the Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
// __END_LICENSE__


#ifndef __VW_PLATE_PLATEFILE_H__
#define __VW_PLATE_PLATEFILE_H__

#include <vw/Math/Vector.h>
#include <vw/Image/ImageView.h>
#include <vw/FileIO/DiskImageResource.h>

#include <vw/Plate/Index.h>
#include <vw/Plate/LocalIndex.h>
#include <vw/Plate/Blob.h>

#include <vector>
#include <fstream>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/convenience.hpp>

// Protocol Buffer
#include <vw/Plate/ProtoBuffers.pb.h>

namespace fs = boost::filesystem;

namespace vw {
namespace platefile {

  // -------------------------------------------------------------------------
  //                            TEMPORARY TILE FILE
  // -------------------------------------------------------------------------

  // A scoped temporary file object that store a tile under /tmp.  The
  // file is destroyed when this object is deleted.
  class TemporaryTileFile {

    std::string m_filename;
    bool m_delete_afterwards;

    // Define these as private methods to enforce TemporaryTileFile'
    // non-copyable semantics.
    TemporaryTileFile() {}
    TemporaryTileFile(TemporaryTileFile const&) {}
    TemporaryTileFile& operator=(TemporaryTileFile const&) { return *this; }
  
  public:

    static std::string unique_tempfile_name(std::string file_extension) {
      char base_name[100] = "/tmp/vw_plate_tile_XXXXXXX";
      std::string name = mktemp(base_name);
      return name + "." + file_extension;
    }

    /// This constructor assumes control over an existing file on disk,
    /// and deletes it when the TemporaryTileFile object is de-allocated.
    TemporaryTileFile(std::string filename, bool delete_afterwards = true) : 
      m_filename(filename), m_delete_afterwards(delete_afterwards) {
      vw_out(DebugMessage, "plate::tempfile") << "Assumed control of temporary file: " 
                                              << m_filename << "\n";

    }

    /// This constructor assumes control over an existing file on disk,
    /// and deletes it when the TemporaryTileFile object is de-allocated.
    template <class ViewT>
    TemporaryTileFile(ImageViewBase<ViewT> const& view, std::string file_extension) : 
      m_filename(unique_tempfile_name(file_extension)), m_delete_afterwards(true) {
      write_image(m_filename, view);
      vw_out(DebugMessage, "plate::tempfile") << "Created temporary file: " 
                                              << m_filename << "\n";
    }

    ~TemporaryTileFile() {
      if (m_delete_afterwards) {
        int result = unlink(m_filename.c_str());
        if (result)
          vw_out(ErrorMessage, "plate::tempfile") 
            << "WARNING: unlink() failed in ~TemporaryTileFile() for filename \"" 
            << m_filename << "\"\n";
        vw_out(DebugMessage, "plate::tempfile") << "Destroyed temporary file: " 
                                                << m_filename << "\n";
      }
    }

    std::string file_name() const { return m_filename; }

    /// Opens the temporary file and determines its size in bytes.
    int64 file_size() const {
      std::ifstream istr(m_filename.c_str(), std::ios::binary);
      
      if (!istr.is_open())
        vw_throw(IOErr() << "TempPlateFile::file_size() -- could not open \"" 
                 << m_filename << "\".");
      istr.seekg(0, std::ios_base::end);
      return istr.tellg();
    }

    template <class PixelT> 
    ImageView<PixelT> read() const {
      ImageView<PixelT> img;
      read_image(img, m_filename);
      return img;
    }
          
  };


  // -------------------------------------------------------------------------
  //                            PLATE FILE
  // -------------------------------------------------------------------------

  
  class PlateFile {
    boost::shared_ptr<Index> m_index;
    FifoWorkQueue m_queue;

  public:

    PlateFile(std::string url, std::string type = "", std::string description = "",
              int tile_size = 256, std::string tile_filetype = "jpg", 
              PixelFormatEnum pixel_format = VW_PIXEL_RGBA,
              ChannelTypeEnum channel_type = VW_CHANNEL_UINT8) {

      // Plate files are stored as an index file and one or more data
      // blob files in a directory.  We create that directory here if
      // it doesn't already exist.
      if( !exists( fs::path( url, fs::native ) ) ) {
        fs::create_directory(url);

        IndexHeader hdr;
        hdr.set_type(type);
        hdr.set_description(description);
        hdr.set_tile_size(tile_size);
        hdr.set_tile_filetype(tile_filetype);
        hdr.set_pixel_format(pixel_format);
        hdr.set_channel_type(channel_type);

        m_index = boost::shared_ptr<Index>( new LocalIndex(url, hdr) );
        vw_out(DebugMessage, "platefile") << "Creating new plate file: \"" << url << "\"\n";

      // However, if it does exist, then we attempt to open the
      // platefile that is stored there.
      } else {
        m_index = boost::shared_ptr<Index>( new LocalIndex(url) );
        vw_out(DebugMessage, "platefile") << "Re-opened plate file: \"" << url << "\"\n";
      }
      
    }


    // // Open an existing platefile
    // PlateFile(std::string url);
  
    // // Create a new platefile
    // PlateFile(std::string url, std::string type, std::string description,
    //           int tile_size = 256, std::string tile_filetype = "jpg", 
    //           PixelFormatEnum pixel_format = VW_PIXEL_RGBA,
    //           ChannelTypeEnum channel_type = VW_CHANNEL_UINT8);

    /// The destructor saves the platefile to disk. 
    ~PlateFile() {}

    /// Returns the name of the root directory containing the plate file.
    std::string name() const { return m_index->platefile_name(); }

    /// Returns the file type used to store tiles in this plate file.
    std::string default_file_type() const { return m_index->tile_filetype(); }

    int default_tile_size() const { return m_index->tile_size(); }

    PixelFormatEnum pixel_format() const { return m_index->pixel_format(); }

    ChannelTypeEnum channel_type() const { return m_index->channel_type(); }

    int depth() const { return m_index->max_depth(); }

    /// Read the tile header. You supply a base name (without the
    /// file's image extension).  The image extension will be appended
    /// automatically for you based on the filetype in the TileHeader.
    std::string read_to_file(std::string const& base_name, 
                             int col, int row, int depth, int transaction_id) {

      TileHeader result;
      std::string filename = base_name;

      // 1. Call index read_request(col,row,depth).  Returns IndexRecord.
      IndexRecord record = m_index->read_request(col, row, depth, transaction_id);
      if (record.status() != INDEX_RECORD_EMPTY) {
        std::ostringstream blob_filename;
        blob_filename << this->name() << "/plate_" << record.blob_id() << ".blob";

        // 2. Open the blob file and read the header
        Blob blob(blob_filename.str());
        TileHeader header = blob.read_header<TileHeader>(record.blob_offset());

        // 3. Choose a temporary filename and call BlobIO
        // read_as_file(filename, offset, size) [ offset, size from
        // IndexRecord ]
        filename += "." + header.filetype();
        blob.read_to_file(filename, record.blob_offset());

        // 4. Return the name of the file
        return filename;
      } else {
        vw_throw(TileNotFoundErr() << "Index record was found, but was marked as empty.");
        return filename; // never reached
      }

    }


    /// Read an image from the specified tile location in the plate file.
    template <class ViewT>
    TileHeader read(ViewT &view, int col, int row, int depth, int transaction_id) {

      TileHeader result;
      
      // 1. Call index read_request(col,row,depth).  Returns IndexRecord.
      IndexRecord record = m_index->read_request(col, row, depth, transaction_id);
      if (record.status() != INDEX_RECORD_EMPTY) {
        std::ostringstream blob_filename;
        blob_filename << this->name() << "/plate_" << record.blob_id() << ".blob";

        // 2. Open the blob file and read the header
        Blob blob(blob_filename.str());
        TileHeader header = blob.read_header<TileHeader>(record.blob_offset());

        // 3. Choose a temporary filename and call BlobIO
        // read_as_file(filename, offset, size) [ offset, size from
        // IndexRecord ]
        std::string tempfile = TemporaryTileFile::unique_tempfile_name(header.filetype());
        blob.read_to_file(tempfile, record.blob_offset());
        TemporaryTileFile tile(tempfile);

        // 4. Read data from temporary file.
        view = tile.read<typename ViewT::pixel_type>();

        // 5. Access the tile header and return it.
        result = blob.read_header<TileHeader>(record.blob_offset());
        return result;
      } else {
        vw_throw(TileNotFoundErr() << "Index record was found, but was marked as empty.");
        return result; // never reached
      }
    }

    /// Write an image to the specified tile location in the plate file.
    template <class ViewT>
    void write(ImageViewBase<ViewT> const& view, 
               int col, int row, int depth, int transaction_id) {      

      // 1. Write data to temporary file. 
      TemporaryTileFile tile(view, this->default_file_type());
      std::string tile_filename = tile.file_name();
      int64 file_size = tile.file_size();

      // 2. Make write_request(size) to index. Returns blob id.
      int blob_id = m_index->write_request(file_size);
      std::ostringstream blob_filename;
      blob_filename << this->name() << "/plate_" << blob_id << ".blob";

      // 3. Create a blob and call write_from_file(filename).  Returns offset, size.
      Blob blob(blob_filename.str());

      TileHeader write_header;
      write_header.set_col(col);
      write_header.set_row(row);
      write_header.set_depth(depth);
      write_header.set_transaction_id(transaction_id);
      write_header.set_filetype(this->default_file_type());

      int64 blob_offset;
      blob.write_from_file(tile_filename, write_header, blob_offset);

      // 4. Call write_complete(col, row, depth, record)

      IndexRecord write_record;
      write_record.set_blob_id(blob_id);
      write_record.set_blob_offset(blob_offset);
      write_record.set_status(INDEX_RECORD_VALID);
      //      write_record.set_tile_size(file_size);  // ????

      m_index->write_complete(write_header, write_record);
    }


    /// Read a record out of the platefile.  
    ///
    /// A transaction ID of -1 indicates that we should return the
    /// most recent tile, regardless of its transaction id.
    IndexRecord read_record(int col, int row, int depth, int transaction_id) {
      return m_index->read_request(col, row, depth, transaction_id);
    }

    // --------------------- TRANSACTIONS ------------------------

    // Clients are expected to make a transaction request whenever
    // they start a self-contained chunk of mosaicking work.  .
    virtual int32 transaction_request(std::string transaction_description) {
      return m_index->transaction_request(transaction_description);
    }

    // Once a chunk of work is complete, clients can "commit" their
    // work to the mosaic by issuding a transaction_complete method.
    virtual void transaction_complete(int32 transaction_id) {
      m_index->transaction_complete(transaction_id);
    }

    virtual int32 transaction_cursor() {
      return m_index->transaction_cursor();
    }
    
    // ----------------------- UTILITIES --------------------------

    void map(boost::shared_ptr<TreeMapFunc> func) {
      m_index->map(func);
    }

  };



}} // namespace vw::plate

#endif // __VW_PLATE_PLATEFILE_H__

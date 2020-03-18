
#pragma once

#include "leveldb/compressor.h"

namespace LevelDB {

	public ref class ZlibCompressorBase : public Compressor 
	{
	public:
		int inflate(const char* input, size_t length, ::System::String &output) const;

		const int compressionLevel;
		const bool raw;
        
        virtual ~ZlibCompressorBase() {
            
        }

		ZlibCompressorBase(char uniqueCompressionID, int compressionLevel, bool raw) :
			Compressor(uniqueCompressionID),
			compressionLevel(compressionLevel),
			raw(raw)
		{
			assert(compressionLevel >= -1 && compressionLevel <= 9);
		}

		virtual void compressImpl(const char* input, size_t length, ::System::String& output) const override;

		virtual bool decompress(const char* input, size_t length, ::System::String &output) const override;

	private:

		int _window() const;

	};

	public ref class ZlibCompressor : public ZlibCompressorBase {
	public:
		static const int SERIALIZE_ID = 2;

		ZlibCompressor(int compressionLevel = -1) :
			ZlibCompressorBase(SERIALIZE_ID, compressionLevel, false) {

		}
	};

	public ref class ZlibCompressorRaw : public ZlibCompressorBase {
	public:
		static const int SERIALIZE_ID = 4;

		ZlibCompressorRaw(int compressionLevel = -1) :
			ZlibCompressorBase(SERIALIZE_ID, compressionLevel, true) {

		}
	};
}
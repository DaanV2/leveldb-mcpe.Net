#pragma once

#include <string>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <memory>

namespace LevelDB {
	public ref class Slice;

	public ref class Compressor
	{
	public:

		uint64_t inputBytes = 0, compressedBytes = 0;
 
		//an ID that has to be unique across the whole system
		const char uniqueCompressionID;

        virtual ~Compressor() {}
        
		Compressor(char uniqueCompressionID) :
			uniqueCompressionID(uniqueCompressionID) {

		}

		double getAverageCompression() const
		{
			return inputBytes ? ((double)compressedBytes / (double)inputBytes) : 0;
		}

		void resetAverageCompressionStats() {
			inputBytes = compressedBytes = 0;
		}

		void compress(const char* input, size_t length, ::System::String& output) {

			compressImpl(input, length, output);

			inputBytes += length;
			compressedBytes += output.length();
		}

		void compress(const System::String& in, System::String& out) {
			compress(in.data(), in.length(), out);
		}

		virtual void compressImpl(const char* input, size_t length, ::System::String& output) const = 0;

		virtual bool decompress(const char* input, size_t length, ::System::String &output) const = 0;

		bool decompress(const System::String& input, ::System::String& output) const {
			return decompress(input.data(), input.length(), output);
		}

	protected:
	private:
	};
}
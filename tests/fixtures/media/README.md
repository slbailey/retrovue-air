# Test Media Assets

This directory contains test video files for VideoFileProducer contract tests.

## Directory Structure

Place your test MP4 files here. Recommended files:

- `test_h264_1080p.mp4` - H.264 encoded, 1920x1080, 30fps, ~10 seconds
- `test_hevc_1080p.mp4` - HEVC encoded, 1920x1080, 30fps, ~10 seconds

## Usage in Tests

To use a real MP4 file in your tests, set the `asset_uri` in `ProducerConfig`:

```cpp
ProducerConfig config;
config.asset_uri = "tests/fixtures/media/test_h264_1080p.mp4";  // Relative path
// OR
config.asset_uri = "/absolute/path/to/your/video.mp4";  // Absolute path
config.stub_mode = false;  // Enable real decoding
```

## Example Test

```cpp
TEST_F(VideoFileProducerContractTest, RealDecodeTest)
{
  ProducerConfig config;
  config.asset_uri = "tests/fixtures/media/test_h264_1080p.mp4";
  config.target_width = 1920;
  config.target_height = 1080;
  config.target_fps = 30.0;
  config.stub_mode = false;  // Use real FFmpeg decoder

  producer_ = std::make_unique<VideoFileProducer>(
      config, *buffer_, clock_, MakeEventCallback());
  
  ASSERT_TRUE(producer_->start());
  // ... test assertions ...
  producer_->stop();
}
```

## Note

- Most contract tests use `stub_mode = true` and don't require actual video files
- Real decode tests require FFmpeg to be available (`RETROVUE_FFMPEG_AVAILABLE`)
- If the file doesn't exist or FFmpeg isn't available, the producer will fall back to stub mode







import wave
import asyncio
# from logger_config import setup_logging
from videosdk import (
    MeetingConfig,
    VideoSDK,
    Stream,
    Participant,
    Meeting,
    MeetingEventHandler,
    ParticipantEventHandler,
)
from vsaiortc.contrib.media import MediaPlayer
import logging

VIDEOSDK_TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcGlrZXkiOiI0N2M3ZTJlYy01NzY5LTQ3OWQtYjdjNS0zYjU5MDcxYzhhMDkiLCJwZXJtaXNzaW9ucyI6WyJhbGxvd19qb2luIl0sImlhdCI6MTY3MjgwOTcxMywiZXhwIjoxODMwNTk3NzEzfQ.KeXr1cxORdq6X7-sxBLLV7MsUnwuJGLaG8_VTyTFBig"
MEETING_ID = "roye-pqdd-wbfl"
NAME = "Python"

print("MEETING ID", MEETING_ID)
# setup_logging()
meeting: Meeting = None
loop = asyncio.get_event_loop()
logging.basicConfig(level=logging.DEBUG)

class AudioRecorder:
    def __init__(self, filename="output.wav", sample_rate=48000, channels=1, sampwidth=2):
        self.filename = filename
        self.sample_rate = sample_rate
        self.channels = channels
        self.sampwidth = sampwidth  # 2 bytes = 16-bit PCM
        self.wavefile = wave.open(filename, 'wb')
        self.wavefile.setnchannels(channels)
        self.wavefile.setsampwidth(sampwidth)
        self.wavefile.setframerate(sample_rate)
        self.queue = asyncio.Queue()
        self._running = True

    async def write_loop(self):
        while self._running:
            frame = await self.queue.get()
            if frame is None:
                break
            self.wavefile.writeframes(frame)

    def stop(self):
        self._running = False
        self.queue.put_nowait(None)
        self.wavefile.close()


class MyMeetingEventHandler(MeetingEventHandler):
    def __init__(self, meeting):
        super().__init__()
        self.meeting: Meeting = meeting

    def on_meeting_joined(self, data):
        print("meeting joined")

    def on_meeting_left(self, data):
        print("meeting left")

    def on_participant_joined(self, p: Participant):
        print("participant joined", p.id)
        p.add_event_listener(MyParticipantEventHandler())

        async def capture_image():
            p.async_capture_image()

    def on_participant_left(self, p: Participant):
        print("participant left", p.id)


class MyParticipantEventHandler(ParticipantEventHandler):
    def __init__(self):
        super().__init__()
        self.audio_recorder = None

    def on_stream_enabled(self, stream: Stream):
        if stream.kind == "audio":
            print("stream-enabled audio.. subscribing & recording")
            self.audio_recorder = AudioRecorder("participant_audio.wav",
                                                sample_rate=48000,
                                                channels=2,  # match OpusDecoder
                                                sampwidth=2)

            asyncio.ensure_future(self.audio_recorder.write_loop())

            # Subscribe to audio frames
            stream.on("data", self.on_audio_frame)

    def on_stream_disabled(self, stream: Stream):
        if stream.kind == "audio":
            print("stream-disabled audio.. stopping recorder")
            if self.audio_recorder:
                self.audio_recorder.stop()
                self.audio_recorder = None

    def on_audio_frame(self, frame):
        """Convert AudioFrame → PCM bytes → write to wav"""
        if isinstance(frame, AudioFrame):
            pcm = frame.planes[0].to_bytes()  # get raw PCM (s16 stereo)
            if self.audio_recorder:
                self.audio_recorder.queue.put_nowait(pcm)
        else:
            print("⚠️ Got non-AudioFrame:", type(frame))

async def main():
    try:
        print("initializing VideoSDK...")
        global meeting
        # player = MediaPlayer("example-video.mp4")
        # Example usage:
        meeting_config = MeetingConfig(
            meeting_id=MEETING_ID,
            name=NAME,
            mic_enabled=False,
            webcam_enabled=False,
            # custom_microphone_audio_track=player.audio,
            # custom_camera_video_track=player.video,
            token=VIDEOSDK_TOKEN,
            signaling_base_url="dev-api.videosdk.live",
        )
        meeting = VideoSDK.init_meeting(**meeting_config)

        print("adding event listener...")
        meeting.add_event_listener(MyMeetingEventHandler(meeting))

        print("joining into meeting...")
        await meeting.async_join()

    except Exception as e:
        print("error", e)


if __name__ == "__main__":
    loop.run_until_complete(main())
    loop.run_forever()
// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// Frame/input tap: see gui/include/tap.h and doc/tap.md.

#include <tap.h>

#include <QLoggingCategory>
#include <QtGlobal>

Q_LOGGING_CATEGORY(chiakiTap, "chiaki.tap", QtInfoMsg);

#if !CHIAKI_GUI_ENABLE_TAP

void ChiakiTap::Init()
{
	if(qEnvironmentVariableIsSet("CHIAKI_TAP_SOCK"))
		qCWarning(chiakiTap) << "CHIAKI_TAP_SOCK is set, but this build has no tap (it needs libswscale and a POSIX system)";
}
void ChiakiTap::OfferInput(const ChiakiControllerState &) {}
void ChiakiTap::OfferFrame(const AVFrame *) {}
void ChiakiTap::SessionStarted(unsigned, unsigned) {}
void ChiakiTap::SessionStopped() {}

#else

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace
{

// Wire protocol, version 1. All integers little-endian; see doc/tap.md.
constexpr uint32_t kMagic = 0x5041544B; // "KTAP" in byte order
constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeHello = 1;
constexpr uint8_t kTypeInput = 2;
constexpr uint8_t kTypeFrame = 3;
constexpr uint8_t kTypeSession = 4;
constexpr uint16_t kFlagPostInput = 1 << 0;
constexpr uint8_t kFormatRGB24 = 1;
constexpr size_t kHeaderSize = 24;
constexpr size_t kInputSize = 16;
constexpr size_t kFrameInfoSize = 16;

constexpr double kDefaultFps = 4.0;
constexpr int64_t kPostInputDelayNs = 150 * 1000 * 1000;
constexpr size_t kFrameQueueMax = 2;
// Inputs and session events are never dropped. A client that leaves this many
// unsent (minutes of stick movement) has stopped reading and is disconnected.
constexpr size_t kControlQueueMax = 16384;
constexpr size_t kPostInputDeadlinesMax = 64;
// A message the client hasn't taken within this long means it stopped
// reading: it is disconnected.
constexpr int64_t kSendDeadlineNs = int64_t(3) * 1000 * 1000 * 1000;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // macOS: SO_NOSIGPIPE on the socket instead
#endif

int64_t NowNs(clockid_t clock)
{
	timespec ts;
	clock_gettime(clock, &ts);
	return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void Put16(uint8_t *p, uint16_t v)
{
	p[0] = uint8_t(v);
	p[1] = uint8_t(v >> 8);
}

void Put32(uint8_t *p, uint32_t v)
{
	for(int i = 0; i < 4; i++)
		p[i] = uint8_t(v >> (8 * i));
}

void Put64(uint8_t *p, uint64_t v)
{
	for(int i = 0; i < 8; i++)
		p[i] = uint8_t(v >> (8 * i));
}

void PutHeader(uint8_t *p, uint8_t type, uint16_t flags, int64_t ts_ns, uint32_t payload_len)
{
	Put32(p, kMagic);
	p[4] = kVersion;
	p[5] = type;
	Put16(p + 6, flags);
	Put64(p + 8, uint64_t(ts_ns));
	Put32(p + 16, payload_len);
	Put32(p + 20, 0);
}

std::vector<uint8_t> Message(uint8_t type, int64_t ts_ns, const void *payload, size_t len)
{
	std::vector<uint8_t> m(kHeaderSize + len);
	PutHeader(m.data(), type, 0, ts_ns, uint32_t(len));
	if(len)
		memcpy(m.data() + kHeaderSize, payload, len);
	return m;
}

// Writes all of iov, resuming after partial writes. Fails on an error, or
// when the client hasn't taken it all within kSendDeadlineNs. The socket's
// SO_SNDTIMEO (1 s) wakes a blocked send to check the deadline.
bool SendAll(int fd, iovec *iov, int count)
{
	const int64_t deadline = NowNs(CLOCK_MONOTONIC) + kSendDeadlineNs;
	while(count > 0)
	{
		msghdr msg = {};
		msg.msg_iov = iov;
		msg.msg_iovlen = count;
		ssize_t n = sendmsg(fd, &msg, kSendFlags);
		if(n < 0)
		{
			if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
				return false;
			n = 0; // interrupted, or SO_SNDTIMEO woke us without progress
		}
		while(count > 0 && size_t(n) >= iov->iov_len)
		{
			n -= ssize_t(iov->iov_len);
			iov++;
			count--;
		}
		if(count > 0)
		{
			iov->iov_base = static_cast<uint8_t *>(iov->iov_base) + n;
			iov->iov_len -= size_t(n);
			if(NowNs(CLOCK_MONOTONIC) > deadline)
			{
				errno = ETIMEDOUT;
				return false;
			}
		}
	}
	return true;
}

bool SendAll(int fd, const std::vector<uint8_t> &m)
{
	iovec iov = { const_cast<uint8_t *>(m.data()), m.size() };
	return SendAll(fd, &iov, 1);
}

// Whether the client is still there. It never sends anything, so readable
// means end of stream (or junk, which is discarded).
bool ClientAlive(int fd)
{
	pollfd p = { fd, POLLIN, 0 };
	if(poll(&p, 1, 0) < 0)
		return errno == EINTR;
	if(p.revents & (POLLERR | POLLHUP | POLLNVAL))
		return false;
	if(p.revents & POLLIN)
	{
		char buf[256];
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		return n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
	}
	return true;
}

// swscale's YUV->RGB matrix for a frame. Untagged HD video is BT.709.
int SwsColorspace(const AVFrame *f)
{
	switch(f->colorspace)
	{
		case AVCOL_SPC_BT709:
			return SWS_CS_ITU709;
		case AVCOL_SPC_FCC:
			return SWS_CS_FCC;
		case AVCOL_SPC_SMPTE240M:
			return SWS_CS_SMPTE240M;
		case AVCOL_SPC_BT2020_NCL:
		case AVCOL_SPC_BT2020_CL:
			return SWS_CS_BT2020;
		case AVCOL_SPC_UNSPECIFIED:
			return f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
		default:
			return SWS_CS_ITU601;
	}
}

struct InputState
{
	uint32_t buttons;
	uint8_t l2, r2;
	int16_t lx, ly, rx, ry;

	bool operator==(const InputState &o) const
	{
		return buttons == o.buttons && l2 == o.l2 && r2 == o.r2 && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry;
	}
};

class TapServer
{
	public:
		static TapServer *Get();

		void OfferInput(const ChiakiControllerState &state);
		void OfferFrame(const AVFrame *frame);
		void Session(bool started, unsigned width, unsigned height);

	private:
		struct QueuedFrame
		{
			AVFrame *frame;
			int64_t ts_ns;
			uint16_t flags;
		};

		TapServer(std::string path, double fps);
		bool Listen();
		void AcceptLoop();
		void WorkerLoop();
		void Adopt(int fd);
		void DropClient(const char *why);
		void ClearQueuesLocked();
		void EnqueueControlLocked(std::vector<uint8_t> msg);
		bool SendFrame(const QueuedFrame &qf);
		bool Convert(const AVFrame *in, int *width, int *height, size_t *stride);

		const std::string path;
		const int64_t sample_period_ns; // 0: no periodic frames, only post-input ones
		int listen_fd = -1;

		// Producers check this before doing any work for a frame.
		std::atomic<bool> connected{false};

		std::mutex mutex;
		std::condition_variable cv;
		int pending_fd = -1; // accepted, not yet adopted by the worker
		bool control_overflow = false;
		std::deque<std::vector<uint8_t>> control; // INPUT and SESSION messages
		std::deque<QueuedFrame> frames;
		std::deque<int64_t> post_input_deadlines; // CLOCK_MONOTONIC
		int64_t last_sample_ns = 0;
		bool have_last_input = false;
		InputState last_input = {};
		bool session_active = false;
		unsigned session_width = 0, session_height = 0;
		uint64_t frames_dropped = 0;

		// Worker thread only.
		int client_fd = -1;
		uint64_t frames_sent = 0;
		AVFrame *sw_frame = nullptr;
		SwsContext *sws = nullptr;
		int sws_setup[5] = {}; // format, width, height, colorspace, range the context is set up for
		uint8_t *rgb = nullptr;
		size_t rgb_size = 0;
		bool logged_convert_failure = false;
};

TapServer::TapServer(std::string path, double fps)
	: path(std::move(path)),
	sample_period_ns(fps > 0 ? int64_t(1e9 / fps) : 0)
{
	sw_frame = av_frame_alloc();
}

TapServer *TapServer::Get()
{
	static TapServer *server = []() -> TapServer * {
		const QByteArray path = qgetenv("CHIAKI_TAP_SOCK");
		if(path.isEmpty())
			return nullptr;
		double fps = kDefaultFps;
		const QByteArray fps_str = qgetenv("CHIAKI_TAP_FPS");
		if(!fps_str.isEmpty())
		{
			bool ok = false;
			double v = fps_str.toDouble(&ok); // C locale, whatever the user's
			if(ok && v >= 0 && v <= 240)
				fps = v;
			else
				qCWarning(chiakiTap) << "Ignoring CHIAKI_TAP_FPS" << fps_str << "(want 0 to 240), using" << kDefaultFps;
		}
		auto *t = new TapServer(path.toStdString(), fps);
		if(!t->Listen())
		{
			delete t;
			return nullptr;
		}
		// The server lives as long as the process, so its threads are detached.
		std::thread(&TapServer::AcceptLoop, t).detach();
		std::thread(&TapServer::WorkerLoop, t).detach();
		qCInfo(chiakiTap) << "Serving frames and input on" << path << "-" << fps << "frames/s plus one after each input change";
		return t;
	}();
	return server;
}

bool TapServer::Listen()
{
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if(path.size() >= sizeof(addr.sun_path))
	{
		qCWarning(chiakiTap) << "CHIAKI_TAP_SOCK is longer than" << sizeof(addr.sun_path) - 1 << "bytes, tap disabled:" << path.c_str();
		return false;
	}
	memcpy(addr.sun_path, path.c_str(), path.size() + 1);

	struct stat st;
	if(lstat(path.c_str(), &st) == 0)
	{
		if(!S_ISSOCK(st.st_mode))
		{
			qCWarning(chiakiTap) << path.c_str() << "exists and is not a socket, so it is left alone and the tap is disabled";
			return false;
		}
		unlink(path.c_str()); // stale, from an earlier run
	}

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(listen_fd < 0)
	{
		qCWarning(chiakiTap) << "socket() failed:" << strerror(errno);
		return false;
	}
	fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
	if(bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0
		|| chmod(path.c_str(), S_IRUSR | S_IWUSR) < 0 // only this user may connect
		|| listen(listen_fd, 4) < 0)
	{
		qCWarning(chiakiTap) << "Can't listen on" << path.c_str() << ":" << strerror(errno);
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	return true;
}

void TapServer::AcceptLoop()
{
	for(;;)
	{
		int fd = accept(listen_fd, nullptr, nullptr);
		if(fd < 0)
		{
			if(errno != EINTR && errno != ECONNABORTED)
			{
				qCWarning(chiakiTap) << "accept() failed:" << strerror(errno);
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
			continue;
		}
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		std::lock_guard<std::mutex> lock(mutex);
		if(pending_fd >= 0)
			close(pending_fd); // superseded before the worker got to it
		pending_fd = fd;
		cv.notify_all();
	}
}

// The worker owns the client socket: it adopts new connections, sends queued
// inputs and session events first, then at most one frame, and repeats.
void TapServer::WorkerLoop()
{
	std::unique_lock<std::mutex> lock(mutex);
	for(;;)
	{
		cv.wait_for(lock, std::chrono::milliseconds(500), [this] {
			return pending_fd >= 0 || control_overflow || (client_fd >= 0 && (!control.empty() || !frames.empty()));
		});
		if(pending_fd >= 0)
		{
			int fd = pending_fd;
			pending_fd = -1;
			lock.unlock();
			Adopt(fd);
			lock.lock();
			continue;
		}
		if(client_fd < 0)
			continue;
		if(control_overflow)
		{
			lock.unlock();
			DropClient("it stopped reading");
			lock.lock();
			continue;
		}
		if(control.empty() && frames.empty())
		{
			// Idle: notice a client that left while there was nothing to send.
			lock.unlock();
			if(!ClientAlive(client_fd))
				DropClient("it disconnected");
			lock.lock();
			continue;
		}

		std::deque<std::vector<uint8_t>> msgs;
		msgs.swap(control);
		QueuedFrame qf = {};
		if(!frames.empty())
		{
			qf = frames.front();
			frames.pop_front();
		}
		lock.unlock();

		bool ok = true;
		for(const auto &m : msgs)
		{
			if(!(ok = SendAll(client_fd, m)))
				break;
		}
		if(ok && qf.frame)
			ok = SendFrame(qf);
		const int send_errno = errno;
		if(qf.frame)
			av_frame_free(&qf.frame);
		if(!ok)
			DropClient(send_errno == ETIMEDOUT ? "it stopped reading" : strerror(send_errno));
		lock.lock();
	}
}

// Takes a new connection: one client at a time, so it is refused while a
// live one is connected.
void TapServer::Adopt(int fd)
{
	if(client_fd >= 0)
	{
		if(ClientAlive(client_fd))
		{
			qCInfo(chiakiTap) << "Refused a client: another one is connected";
			close(fd);
			return;
		}
		DropClient("it disconnected");
	}
	int one = 1;
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	(void)one;
	timeval timeout = {};
	timeout.tv_sec = 1;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	// HELLO goes out before the producers start queueing for this client.
	const std::string hello = std::string("{\"version\":1,\"chiaki\":\"") + CHIAKI_VERSION + "\"}";
	if(!SendAll(fd, Message(kTypeHello, NowNs(CLOCK_REALTIME), hello.data(), hello.size())))
	{
		qCInfo(chiakiTap) << "Client left before HELLO:" << strerror(errno);
		close(fd);
		return;
	}
	client_fd = fd;
	frames_sent = 0;
	{
		std::lock_guard<std::mutex> lock(mutex);
		ClearQueuesLocked();
		frames_dropped = 0;
		last_sample_ns = 0;
		connected.store(true);
	}
	qCInfo(chiakiTap) << "Client connected";
}

void TapServer::DropClient(const char *why)
{
	uint64_t dropped;
	{
		std::lock_guard<std::mutex> lock(mutex);
		connected.store(false);
		ClearQueuesLocked();
		dropped = frames_dropped;
	}
	close(client_fd);
	client_fd = -1;
	qCInfo(chiakiTap).nospace() << "Client dropped (" << why << ") after " << frames_sent << " frames, " << dropped << " dropped for backpressure";
}

void TapServer::ClearQueuesLocked()
{
	control.clear();
	control_overflow = false;
	for(auto &qf : frames)
		av_frame_free(&qf.frame);
	frames.clear();
	post_input_deadlines.clear();
}

void TapServer::EnqueueControlLocked(std::vector<uint8_t> msg)
{
	if(control.size() >= kControlQueueMax)
	{
		control_overflow = true; // the worker disconnects the client
		control.clear();
	}
	else
		control.push_back(std::move(msg));
	cv.notify_all();
}

void TapServer::OfferInput(const ChiakiControllerState &state)
{
	const InputState in = { state.buttons, state.l2_state, state.r2_state, state.left_x, state.left_y, state.right_x, state.right_y };
	std::lock_guard<std::mutex> lock(mutex);
	const bool changed = !have_last_input || !(in == last_input);
	last_input = in;
	have_last_input = true;
	if(!changed || !connected.load(std::memory_order_relaxed))
		return;

	uint8_t p[kInputSize];
	Put32(p, in.buttons);
	p[4] = in.l2;
	p[5] = in.r2;
	Put16(p + 6, uint16_t(in.lx));
	Put16(p + 8, uint16_t(in.ly));
	Put16(p + 10, uint16_t(in.rx));
	Put16(p + 12, uint16_t(in.ry));
	Put16(p + 14, 0);
	EnqueueControlLocked(Message(kTypeInput, NowNs(CLOCK_REALTIME), p, sizeof(p)));

	// The first frame decoded 150 ms from now shows what this input did.
	const int64_t deadline = NowNs(CLOCK_MONOTONIC) + kPostInputDelayNs;
	if(post_input_deadlines.size() < kPostInputDeadlinesMax)
		post_input_deadlines.push_back(deadline);
	else
		post_input_deadlines.back() = deadline;
}

void TapServer::OfferFrame(const AVFrame *frame)
{
	if(!frame || !connected.load(std::memory_order_relaxed))
		return;
	const int64_t now = NowNs(CLOCK_MONOTONIC);
	const int64_t ts = NowNs(CLOCK_REALTIME);
	uint16_t flags = 0;
	{
		std::lock_guard<std::mutex> lock(mutex);
		bool post_input = false;
		while(!post_input_deadlines.empty() && post_input_deadlines.front() <= now)
		{
			post_input_deadlines.pop_front();
			post_input = true;
		}
		const bool sample = sample_period_ns > 0 && now - last_sample_ns >= sample_period_ns;
		if(!sample && !post_input)
			return;
		if(sample)
			last_sample_ns = now;
		if(post_input)
			flags |= kFlagPostInput;
	}

	AVFrame *ref = av_frame_clone(frame); // a new reference, no pixel copy
	if(!ref)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if(!connected.load(std::memory_order_relaxed))
	{
		av_frame_free(&ref);
		return;
	}
	if(frames.size() >= kFrameQueueMax)
	{
		// The worker is behind: drop the oldest frame, sparing post-input ones.
		auto victim = std::find_if(frames.begin(), frames.end(), [](const QueuedFrame &f) { return !(f.flags & kFlagPostInput); });
		if(victim == frames.end())
			victim = frames.begin();
		av_frame_free(&victim->frame);
		frames.erase(victim);
		frames_dropped++;
	}
	frames.push_back({ ref, ts, flags });
	cv.notify_all();
}

void TapServer::Session(bool started, unsigned width, unsigned height)
{
	std::lock_guard<std::mutex> lock(mutex);
	if(started)
	{
		session_active = true;
		session_width = width;
		session_height = height;
	}
	else
	{
		if(!session_active)
			return;
		session_active = false;
		width = session_width;
		height = session_height;
	}
	if(!connected.load(std::memory_order_relaxed))
		return;
	char json[96];
	int n = snprintf(json, sizeof(json), "{\"event\":\"%s\",\"width\":%u,\"height\":%u}", started ? "started" : "stopped", width, height);
	EnqueueControlLocked(Message(kTypeSession, NowNs(CLOCK_REALTIME), json, size_t(n)));
}

bool TapServer::SendFrame(const QueuedFrame &qf)
{
	int width, height;
	size_t stride;
	if(!Convert(qf.frame, &width, &height, &stride))
		return true; // skip the frame, keep the client
	const size_t pixels = stride * size_t(height);
	uint8_t head[kHeaderSize + kFrameInfoSize];
	PutHeader(head, kTypeFrame, qf.flags, qf.ts_ns, uint32_t(kFrameInfoSize + pixels));
	uint8_t *info = head + kHeaderSize;
	Put32(info, uint32_t(width));
	Put32(info + 4, uint32_t(height));
	Put32(info + 8, uint32_t(stride));
	info[12] = kFormatRGB24;
	info[13] = info[14] = info[15] = 0;
	iovec iov[2] = { { head, sizeof(head) }, { rgb, pixels } };
	if(!SendAll(client_fd, iov, 2))
		return false;
	frames_sent++;
	return true;
}

// Converts in to RGB24 at its native size, into rgb. Hardware frames
// (VideoToolbox on macOS) are first copied to system memory.
bool TapServer::Convert(const AVFrame *in, int *width, int *height, size_t *stride)
{
	const AVFrame *src = in;
	if(in->hw_frames_ctx)
	{
		if(sw_frame)
			av_frame_unref(sw_frame);
		if(!sw_frame || av_hwframe_transfer_data(sw_frame, in, 0) < 0)
		{
			if(!logged_convert_failure)
				qCWarning(chiakiTap) << "Can't copy hardware frames to memory; skipping them";
			logged_convert_failure = true;
			return false;
		}
		av_frame_copy_props(sw_frame, in); // colour space and range
		src = sw_frame;
	}
	if(src->width <= 0 || src->height <= 0)
		return false;
	*width = src->width;
	*height = src->height;
	*stride = (size_t(src->width) * 3 + 31) & ~size_t(31);
	const size_t need = *stride * size_t(src->height);
	if(need > rgb_size)
	{
		av_free(rgb);
		rgb = static_cast<uint8_t *>(av_malloc(need));
		rgb_size = rgb ? need : 0;
		if(!rgb)
			return false;
	}

	const int setup[5] = { src->format, src->width, src->height, SwsColorspace(src), src->color_range };
	if(!sws || memcmp(setup, sws_setup, sizeof(setup)) != 0)
	{
		sws = sws_getCachedContext(sws, src->width, src->height, static_cast<AVPixelFormat>(src->format),
				src->width, src->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if(!sws)
		{
			if(!logged_convert_failure)
				qCWarning(chiakiTap) << "swscale can't convert pixel format" << src->format << "to RGB24; skipping frames";
			logged_convert_failure = true;
			memset(sws_setup, 0, sizeof(sws_setup));
			return false;
		}
		sws_setColorspaceDetails(sws, sws_getCoefficients(setup[3]), src->color_range == AVCOL_RANGE_JPEG,
				sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
		memcpy(sws_setup, setup, sizeof(setup));
	}
	uint8_t *dst[4] = { rgb, nullptr, nullptr, nullptr };
	int dst_stride[4] = { int(*stride), 0, 0, 0 };
	sws_scale(sws, src->data, src->linesize, 0, src->height, dst, dst_stride);
	return true;
}

} // namespace

void ChiakiTap::Init()
{
	TapServer::Get();
}

void ChiakiTap::OfferInput(const ChiakiControllerState &state)
{
	if(TapServer *tap = TapServer::Get())
		tap->OfferInput(state);
}

void ChiakiTap::OfferFrame(const AVFrame *frame)
{
	if(TapServer *tap = TapServer::Get())
		tap->OfferFrame(frame);
}

void ChiakiTap::SessionStarted(unsigned width, unsigned height)
{
	if(TapServer *tap = TapServer::Get())
		tap->Session(true, width, height);
}

void ChiakiTap::SessionStopped()
{
	if(TapServer *tap = TapServer::Get())
		tap->Session(false, 0, 0);
}

#endif // CHIAKI_GUI_ENABLE_TAP

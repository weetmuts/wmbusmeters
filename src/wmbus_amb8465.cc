/*
 Copyright (C) 2018-2020 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include"wmbus.h"
#include"wmbus_utils.h"
#include"wmbus_amb8465.h"
#include"serial.h"

#include<assert.h>
#include<pthread.h>
#include<semaphore.h>
#include<sys/errno.h>
#include<unistd.h>
#include<sys/time.h>
#include<time.h>

using namespace std;

struct WMBusAmber : public virtual WMBusCommonImplementation
{
    bool ping();
    uint32_t getDeviceId();
    LinkModeSet getLinkModes();
    void setLinkModes(LinkModeSet lms);
    LinkModeSet supportedLinkModes()
    {
        return
            C1_bit |
            S1_bit |
            S1m_bit |
            T1_bit;
    }
    int numConcurrentLinkModes() { return 1; }
    bool canSetLinkModes(LinkModeSet desired_modes)
    {
        if (0 == countSetBits(desired_modes.bits())) return false;
        // Simple check first, are they all supported?
        if (!supportedLinkModes().supports(desired_modes)) return false;
        // So far so good, is the desired combination supported?
        // If only a single bit is desired, then it is supported.
        if (1 == countSetBits(desired_modes.bits())) return true;
        // More than 2 listening modes at the same time will always fail.
        if (2 != countSetBits(desired_modes.bits())) return false;
        // C1 and T1 can be listened to at the same time!
        if (desired_modes.has(LinkMode::C1) && desired_modes.has(LinkMode::T1)) return true;
        // Likewise for S1 and S1-m
        if (desired_modes.has(LinkMode::S1) || desired_modes.has(LinkMode::S1m)) return true;
        // Any other combination is forbidden.
        return false;
    }
    void processSerialData();
    void getConfiguration();
    SerialDevice *serial() { return serial_.get(); }
    void simulate() { }

    WMBusAmber(unique_ptr<SerialDevice> serial, SerialCommunicationManager *manager);
    ~WMBusAmber() { }

private:
    unique_ptr<SerialDevice> serial_;
    SerialCommunicationManager *manager_;
    vector<uchar> read_buffer_;
    pthread_mutex_t command_lock_ = PTHREAD_MUTEX_INITIALIZER;
    sem_t command_wait_;
    int sent_command_ {};
    int received_command_ {};
    LinkModeSet link_modes_;
    vector<uchar> received_payload_;
    bool rssi_expected_;
    struct timeval timestamp_last_rx_;

    void waitForResponse();
    FrameStatus checkAMB8465Frame(vector<uchar> &data,
                                  size_t *frame_length,
                                  int *msgid_out,
                                  int *payload_len_out,
                                  int *payload_offset,
                                  uchar *rssi);
    void handleMessage(int msgid, vector<uchar> &frame);
};

unique_ptr<WMBus> openAMB8465(string device, SerialCommunicationManager *manager, unique_ptr<SerialDevice> serial_override)
{
    if (serial_override)
    {
        WMBusAmber *imp = new WMBusAmber(std::move(serial_override), manager);
        return unique_ptr<WMBus>(imp);
    }

    auto serial = manager->createSerialDeviceTTY(device.c_str(), 9600);
    WMBusAmber *imp = new WMBusAmber(std::move(serial), manager);
    return unique_ptr<WMBus>(imp);
}

WMBusAmber::WMBusAmber(unique_ptr<SerialDevice> serial, SerialCommunicationManager *manager) :
    WMBusCommonImplementation(DEVICE_AMB8465), serial_(std::move(serial)), manager_(manager)
{
    sem_init(&command_wait_, 0, 0);
    manager_->listenTo(serial_.get(),call(this,processSerialData));
    serial_->open(true);
    rssi_expected_ = true;
    timerclear(&timestamp_last_rx_);
}

uchar xorChecksum(vector<uchar> msg, int len)
{
    uchar c = 0;
    for (int i=0; i<len; ++i) {
        c ^= msg[i];
    }
    return c;
}

bool WMBusAmber::ping()
{
    if (serial_->readonly()) return true; // Feeding from stdin or file.

    pthread_mutex_lock(&command_lock_);
    // Ping it...
    pthread_mutex_unlock(&command_lock_);

    return true;
}

uint32_t WMBusAmber::getDeviceId()
{
    if (serial_->readonly()) { return 0; }  // Feeding from stdin or file.

    pthread_mutex_lock(&command_lock_);

    vector<uchar> msg(4);
    msg[0] = AMBER_SERIAL_SOF;
    msg[1] = CMD_SERIALNO_REQ;
    msg[2] = 0; // No payload
    msg[3] = xorChecksum(msg, 3);

    assert(msg[3] == 0xf4);

    sent_command_ = CMD_SERIALNO_REQ;
    verbose("(amb8465) get device id\n");
    bool sent = serial()->send(msg);

    uint32_t id = 0;

    if (sent)
    {
        waitForResponse();

        if (received_command_ == (CMD_SERIALNO_REQ | 0x80))
        {
            id = received_payload_[4] << 24 |
                received_payload_[5] << 16 |
                received_payload_[6] << 8 |
                received_payload_[7];
            verbose("(amb8465) device id %08x\n", id);
        }
    }

    pthread_mutex_unlock(&command_lock_);
    return id;
}

LinkModeSet WMBusAmber::getLinkModes()
{
    if (serial_->readonly()) { return Any_bit; }  // Feeding from stdin or file.

    // It is not possible to read the volatile mode set using setLinkModeSet below.
    // (It is possible to read the non-volatile settings, but this software
    // does not change those.) So we remember the state for the device.
    getConfiguration();
    return link_modes_;
}

void WMBusAmber::getConfiguration()
{
    pthread_mutex_lock(&command_lock_);

    vector<uchar> msg(6);
    msg[0] = AMBER_SERIAL_SOF;
    msg[1] = CMD_GET_REQ;
    msg[2] = 0x02;
    msg[3] = 0x00;
    msg[4] = 0x80;
    msg[5] = xorChecksum(msg, 5);

    assert(msg[5] == 0x77);

    verbose("(amb8465) get config\n");
    bool sent = serial()->send(msg);

    if (!sent)
    {
        pthread_mutex_unlock(&command_lock_);
        return;
    }

    waitForResponse();

    if (received_command_ == (0x80|CMD_GET_REQ))
    {
        // These are the non-volatile values stored inside the dongle.
        // However the link mode, radio channel etc might not be the one
        // that we are actually using! Since setting the link mode
        // is possible without changing the non-volatile memory.
        // But there seems to be no way of reading out the set link mode....???
        // Ie there is a disconnect between the flash and the actual running dongle.
        // Oh well.
        //
        // These are just some random config settings store in non-volatile memory.
        verbose("(amb8465) config: uart %02x\n", received_payload_[2]);
        verbose("(amb8465) config: radio Channel %02x\n", received_payload_[60+2]);
        uchar re = received_payload_[69+2];
        verbose("(amb8465) config: rssi enabled %02x\n", re);
        if (re != 0) {
            rssi_expected_ = true;
        }
        verbose("(amb8465) config: mode Preselect %02x\n", received_payload_[70+2]);
    }

    pthread_mutex_unlock(&command_lock_);
}

void WMBusAmber::setLinkModes(LinkModeSet lms)
{
    if (serial_->readonly()) return; // Feeding from stdin or file.

    if (!canSetLinkModes(lms))
    {
        string modes = lms.hr();
        error("(amb8465) setting link mode(s) %s is not supported for amb8465\n", modes.c_str());
    }

    pthread_mutex_lock(&command_lock_);

    vector<uchar> msg(8);
    msg[0] = AMBER_SERIAL_SOF;
    msg[1] = CMD_SET_MODE_REQ;
    sent_command_ = msg[1];
    msg[2] = 1; // Len
    if (lms.has(LinkMode::C1) && lms.has(LinkMode::T1))
    {
        // Listening to both C1 and T1!
        msg[3] = 0x09;
    }
    else if (lms.has(LinkMode::C1))
    {
        // Listening to only C1.
        msg[3] = 0x0E;
    }
    else if (lms.has(LinkMode::T1))
    {
        // Listening to only T1.
        msg[3] = 0x08;
    }
    else if (lms.has(LinkMode::S1) || lms.has(LinkMode::S1m))
    {
        // Listening only to S1 and S1-m
        msg[3] = 0x03;
    }
    msg[4] = xorChecksum(msg, 4);

    verbose("(amb8465) set link mode %02x\n", msg[3]);
    bool sent = serial()->send(msg);

    if (sent) waitForResponse();

    link_modes_ = lms;
    pthread_mutex_unlock(&command_lock_);
}

void WMBusAmber::waitForResponse()
{
    while (manager_->isRunning())
    {
        int rc = sem_wait(&command_wait_);
        if (rc==0) break;
        if (rc==-1)
        {
            if (errno==EINTR) continue;
            break;
        }
    }
}

FrameStatus WMBusAmber::checkAMB8465Frame(vector<uchar> &data,
                                          size_t *frame_length,
                                          int *msgid_out,
                                          int *payload_len_out,
                                          int *payload_offset,
                                          uchar *rssi)
{
    if (data.size() == 0) return PartialFrame;
    debugPayload("(amb8465) checkAMB8465Frame", data);
    int payload_len = 0;
    if (data[0] == 0xff) {
        if (data.size() < 3)
        {
            debug("(amb8465) not enough bytes yet for command.\n");
            return PartialFrame;
        }
        // A command response begins with 0xff
        *msgid_out = data[1];
        payload_len = data[2];
        *payload_len_out = payload_len;
        *payload_offset = 3;
        *frame_length = 3+payload_len + (int)rssi_expected_;
        if (data.size() < *frame_length)
        {
            debug("(amb8465) not enough bytes yet, partial command response %d %d.\n", data.size(), *frame_length);
            return PartialFrame;
        }

        if (rssi_expected_) {
            *rssi = data[*frame_length-1];
        }
        debug("(amb8465) received full command frame\n");
        return FullFrame;
    }
    // If it is not a 0xff we assume it is a message beginning with a length.
    // There might be a different mode where the data is wrapped in 0xff. But for the moment
    // this is what I see.
    payload_len = data[0];
    *msgid_out = 0; // 0 is used to signal
    *payload_len_out = payload_len;
    *payload_offset = 1;
    *frame_length = payload_len+1;
    if (data.size() < *frame_length)
    {
        debug("(amb8465) not enough bytes yet, partial frame %d %d.\n", data.size(), *frame_length);
        return PartialFrame;
    }

    debug("(amb8465) received full frame\n");
    return FullFrame;
}

void WMBusAmber::processSerialData()
{
    vector<uchar> data;

    // Receive and accumulated serial data until a full frame has been received.
    serial_->receive(&data);

    struct timeval timestamp;

    // Check long delay beetween rx chunks
    gettimeofday(&timestamp, NULL);
    if (read_buffer_.size() > 0 && timerisset(&timestamp_last_rx_)) {
        struct timeval chunk_time;
        timersub(&timestamp, &timestamp_last_rx_, &chunk_time);

        if (chunk_time.tv_sec >= 2) {
            debug("(amb8465) rx long delay, clean start\n");
            read_buffer_.clear();
        }
        else
        {
            unsigned long chunk_time_ms = 1000 * chunk_time.tv_sec + chunk_time.tv_usec / 1000;
            debug("(amb8465) chunk time %ld msec\n", chunk_time_ms);
        }
    }

    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

    size_t frame_length;
    int msgid;
    int payload_len, payload_offset;
    uchar rssi;

    for (;;)
    {
        FrameStatus status = checkAMB8465Frame(read_buffer_, &frame_length, &msgid, &payload_len, &payload_offset, &rssi);

        if (status == PartialFrame)
        {
            if (read_buffer_.size() > 0) {
                // Save timestamp of this chunk
                timestamp_last_rx_ = timestamp;
            }
            else
            {
                // Clean and empty
                timerclear(&timestamp_last_rx_);
            }
            break;
        }
        if (status == ErrorInFrame)
        {
            verbose("(amb8465) protocol error in message received!\n");
            string msg = bin2hex(read_buffer_);
            debug("(amb8465) protocol error \"%s\"\n", msg.c_str());
            read_buffer_.clear();
            break;
        }
        if (status == FullFrame)
        {
            vector<uchar> payload;
            if (payload_len > 0)
            {
                uchar l = payload_len;
                payload.insert(payload.end(), &l, &l+1); // Re-insert the len byte.
                payload.insert(payload.end(), read_buffer_.begin()+payload_offset, read_buffer_.begin()+payload_offset+payload_len);
            }

            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin()+frame_length);

            if (rssi_expected_)
            {
                verbose("(amb8465) rssi %d\n", rssi);
            }
            handleMessage(msgid, payload);
        }
    }
}

void WMBusAmber::handleMessage(int msgid, vector<uchar> &frame)
{
    switch (msgid) {
    case (0):
    {
        handleTelegram(frame);
        break;
    }
    case (0x80|CMD_SET_MODE_REQ):
    {
        verbose("(amb8465) set link mode completed\n");
        received_command_ = msgid;
        received_payload_.clear();
        received_payload_.insert(received_payload_.end(), frame.begin(), frame.end());
        debugPayload("(amb8465) set link mode response", received_payload_);
        sem_post(&command_wait_);
        break;
    }
    case (0x80|CMD_GET_REQ):
    {
        verbose("(amb8465) get config completed\n");
        received_command_ = msgid;
        received_payload_.clear();
        received_payload_.insert(received_payload_.end(), frame.begin(), frame.end());
        debugPayload("(amb8465) get config response", received_payload_);
        sem_post(&command_wait_);
        break;
    }
    case (0x80|CMD_SERIALNO_REQ):
    {
        verbose("(amb8465) get device id completed\n");
        received_command_ = msgid;
        received_payload_.clear();
        received_payload_.insert(received_payload_.end(), frame.begin(), frame.end());
        debugPayload("(amb8465) get device id response", received_payload_);
        sem_post(&command_wait_);
        break;
    }
    default:
        verbose("(amb8465) unhandled device message %d\n", msgid);
        received_payload_.clear();
        received_payload_.insert(received_payload_.end(), frame.begin(), frame.end());
        debugPayload("(amb8465) unknown response", received_payload_);
    }
}

bool detectAMB8465(string device, SerialCommunicationManager *manager)
{
    // Talk to the device and expect a very specific answer.
    auto serial = manager->createSerialDeviceTTY(device.c_str(), 9600);
    bool ok = serial->open(false);
    if (!ok) return false;

    vector<uchar> data;
    // First clear out any data in the queue.
    serial->receive(&data);
    data.clear();

    vector<uchar> msg(4);
    msg[0] = AMBER_SERIAL_SOF;
    msg[1] = CMD_SERIALNO_REQ;
    msg[2] = 0; // No payload
    msg[3] = xorChecksum(msg, 3);

    assert(msg[3] == 0xf4);

    verbose("(amb8465) are you there?\n");
    serial->send(msg);
    // Wait for 100ms so that the USB stick have time to prepare a response.
    usleep(1000*100);
    serial->receive(&data);
    int limit = 0;
    while (data.size() > 8 && data[0] != 0xff) {
        // Eat bytes until a 0xff appears to get in sync with the proper response.
        // Extraneous bytes might be due to a partially read telegram.
        data.erase(data.begin());
        vector<uchar> more;
        serial->receive(&more);
        if (more.size() > 0) {
            data.insert(data.end(), more.begin(), more.end());
        }
        if (limit++ > 100) break; // Do not wait too long.
    }

    serial->close();

    if (data.size() < 8 ||
        data[0] != 0xff ||
        data[1] != (0x80 | msg[1]) ||
        data[2] != 0x04 ||
        data[7] != xorChecksum(data, 7)) {
        return false;
    }
    return true;
}

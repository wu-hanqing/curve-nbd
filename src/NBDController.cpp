/*
 *     Copyright (c) 2020 NetEase Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Project: curve
 * Created Date: Tuesday April 21st 2020
 * Author: yangyaokai
 */

/*
 * rbd-nbd - RBD in userspace
 *
 * Copyright (C) 2015 - 2016 Kylin Corporation
 *
 * Author: Yunchuan Wen <yunchuan.wen@kylin-cloud.com>
 *         Li Wang <li.wang@kylin-cloud.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
*/

#include "nbd/src/NBDController.h"

namespace curve {
namespace nbd {

int g_nbd_index;

int IOController::InitDevAttr(NBDConfig* config,
                              uint64_t size,
                              uint32_t blocksize,
                              uint64_t flags) {
    int ret = -1;

    do {
        ret = ioctl(nbdFd_, NBD_SET_BLKSIZE, blocksize);
        if (ret < 0) {
            break;
        }

        ret = ioctl(nbdFd_, NBD_SET_SIZE, size);
        if (ret < 0) {
            break;
        }

        ioctl(nbdFd_, NBD_SET_FLAGS, flags);

        ret = CheckSetReadOnly(nbdFd_, flags);
        if (ret < 0) {
            dout << "curve-nbd: Check and set read only flag failed."
                 << cpp_strerror(ret) << std::endl;
            break;
        }

        if (config->timeout >= 0) {
            ret = ioctl(nbdFd_, NBD_SET_TIMEOUT, (unsigned long)config->timeout);  // NOLINT
            if (ret < 0) {
                dout << "curve-nbd: failed to set timeout: "
                     << cpp_strerror(ret) << std::endl;
                break;
            }
        }
    } while (false);

    return ret;
}

int IOController::MapOnUnusedNbdDevice(int sockfd, std::string* devpath) {
    int index = 0;
    char dev[64];
    const int nbdsMax = get_nbd_max_count();

    while (index < nbdsMax) {
        snprintf(dev, sizeof(dev), "/dev/nbd%d", index);

        int ret = MapOnNbdDeviceByDevPath(sockfd, dev, false);
        if (ret < 0) {
            ++index;
            continue;
        } else {
            *devpath = dev;
            return 0;
        }
    }

    dout << "curve-nbd: failed to map on unused device, max nbd index: "
         << (nbdsMax - 1) << ", last try nbd index: " << (index - 1)
         << ", last error: " << cpp_strerror(errno) << std::endl;

    return -1;
}

int IOController::MapOnNbdDeviceByDevPath(int sockfd,
                                          const std::string& devpath,
                                          bool logWhenError) {
    int index = parse_nbd_index(devpath);
    if (index < 0) {
        return -1;
    }

    int devfd = open(devpath.c_str(), O_RDWR);
    if (devfd < 0) {
        if (logWhenError) {
            dout << "curve-nbd: failed to open device: " << devfd
                 << ", error = " << cpp_strerror(errno) << std::endl;
        }
        return -1;
    }

    int ret = ioctl(devfd, NBD_SET_SOCK, sockfd);
    if (ret < 0) {
        if (logWhenError) {
            dout << "curve-nbd: ioctl NBD_SET_SOCK failed, devpath: " << devpath
                 << ", error = " << cpp_strerror(errno) << std::endl;
        }
        close(devfd);
        return -1;
    }

    nbdFd_ = devfd;
    nbdIndex_ = index;
    g_nbd_index = nbdIndex_;
    return 0;
}

int IOController::SetUp(NBDConfig* config,
                        int sockfd,
                        uint64_t size,
                        uint32_t blocksize,
                        uint64_t flags) {
    int ret = -1;

    if (config->devpath.empty()) {
        ret = MapOnUnusedNbdDevice(sockfd, &config->devpath);
    } else {
        ret = MapOnNbdDeviceByDevPath(sockfd, config->devpath);
    }

    if (ret < 0) {
        return -1;
    }

    do {
        ret = InitDevAttr(config, size, blocksize, flags);
        if (ret < 0) {
            break;
        }

        ret = check_device_size(nbdIndex_, size);
        if (ret < 0) {
            break;
        }

        ret = check_block_size(nbdIndex_, blocksize);
    } while (0);

    if (ret < 0) {
        dout << "curve-nbd: failed to map, status: "
             << cpp_strerror(ret) << std::endl;
        ioctl(nbdFd_, NBD_CLEAR_SOCK);
        close(nbdFd_);
        nbdFd_ = -1;
        nbdIndex_ = -1;
        return ret;
    }

    return 0;
}

int IOController::DisconnectByPath(const std::string& devpath) {
    int devfd = open(devpath.c_str(), O_RDWR);
    if (devfd < 0) {
        dout << "curve-nbd: failed to open device: "
             << devpath << ", error = " << cpp_strerror(errno) << std::endl;
        return devfd;
    }

    int ret = ioctl(devfd, NBD_DISCONNECT);
    if (ret < 0) {
        dout << "curve-nbd: the device is not used. "
             << cpp_strerror(errno) << std::endl;
    }

    close(devfd);
    return ret;
}

int IOController::Resize(uint64_t size) {
    if (nbdFd_ < 0) {
        dout << "resize failed: nbd controller is not setup." << std::endl;
        return -1;
    }
    int ret = ioctl(nbdFd_, NBD_SET_SIZE, size);
    if (ret < 0) {
        dout << "resize failed: " << cpp_strerror(errno) << std::endl;
    }
    return ret;
}

int NetLinkController::Init() {
    if (sock_ != nullptr) {
        dout << "curve-nbd: Could not allocate netlink socket." << std::endl;
        return 0;
    }

    struct nl_sock* sock = nl_socket_alloc();
    if (sock == nullptr) {
        dout << "curve-nbd: Could not alloc netlink socket. Error "
             << cpp_strerror(errno) << std::endl;
        return -1;
    }

    int ret = genl_connect(sock);
    if (ret < 0) {
        dout << "curve-nbd: Could not connect netlink socket. Error "
             << nl_geterror(ret) << std::endl;
        nl_socket_free(sock);
        return -1;
    }

    nlId_ = genl_ctrl_resolve(sock, "nbd");
    if (nlId_ < 0) {
        dout << "curve-nbd: Could not resolve netlink socket. Error "
             << nl_geterror(nlId_) << std::endl;
        nl_close(sock);
        nl_socket_free(sock);
        return -1;
    }
    sock_ = sock;
    return 0;
}

void NetLinkController::Uninit() {
    if (sock_ == nullptr)
        return;

    nl_close(sock_);
    nl_socket_free(sock_);
    sock_ = nullptr;
    nlId_ = -1;
}

int NetLinkController::SetUp(NBDConfig* config,
                             int sockfd,
                             uint64_t size,
                             uint32_t blocksize,
                             uint64_t flags) {
    int ret = Init();
    if (ret < 0) {
        dout << "curve-nbd: Netlink interface not supported."
             << " Using ioctl interface." << std::endl;
        return ret;
    }

    ret = ConnectInternal(config, sockfd, size, blocksize, flags);
    Uninit();
    if (ret < 0) {
        return ret;
    }

    int index = parse_nbd_index(config->devpath);
    if (index < 0) {
        return index;
    }
    ret = check_block_size(index, blocksize);
    if (ret < 0) {
        return ret;
    }
    ret = check_device_size(index, size);
    if (ret < 0) {
        return ret;
    }

    int fd = open(config->devpath.c_str(), O_RDWR);
    if (fd < 0) {
        dout << "curve-nbd: failed to open device: "
             << config->devpath << std::endl;
        return fd;
    }

    ret = CheckSetReadOnly(fd, flags);
    if (ret < 0) {
        dout << "curve-nbd: Check and set read only flag failed."
             << std::endl;
        close(fd);
        return ret;
    }

    nbdFd_ = fd;
    nbdIndex_ = index;
    return 0;
}

int NetLinkController::DisconnectByPath(const std::string& devpath) {
    int index = parse_nbd_index(devpath);
    if (index < 0) {
        return index;
    }

    int ret = Init();
    if (ret < 0) {
        dout << "curve-nbd: Netlink interface not supported."
             << " Using ioctl interface." << std::endl;
        return ret;
    }

    ret = DisconnectInternal(index);
    Uninit();
    return ret;
}

int NetLinkController::Resize(uint64_t size) {
    if (nbdIndex_ < 0) {
        dout << "resize failed: nbd controller is not setup." << std::endl;
        return -1;
    }

    int ret = Init();
    if (ret < 0) {
        dout << "curve-nbd: Netlink interface not supported."
             << " Using ioctl interface." << std::endl;
        return ret;
    }

    ret = ResizeInternal(nbdIndex_, size);
    Uninit();
    return ret;
}

bool NetLinkController::Support() {
    int ret = Init();
    if (ret < 0) {
        dout << "curve-nbd: Netlink interface not supported."
             << " Using ioctl interface." << std::endl;
        return false;
    }
    Uninit();
    return true;
}

static int netlink_connect_cb(struct nl_msg *msg, void *arg) {
    struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));
    NBDConfig *cfg = reinterpret_cast<NBDConfig *>(arg);
    struct nlattr *msg_attr[NBD_ATTR_MAX + 1];
    int ret;

    ret = nla_parse(msg_attr, NBD_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
                    genlmsg_attrlen(gnlh, 0), NULL);
    if (ret) {
        dout << "curve-nbd: Unsupported netlink reply" << std::endl;
        return -NLE_MSGTYPE_NOSUPPORT;
    }

    if (!msg_attr[NBD_ATTR_INDEX]) {
        dout << "curve-nbd: netlink connect reply missing device index."
             << std::endl;
        return -NLE_MSGTYPE_NOSUPPORT;
    }

    uint32_t index = nla_get_u32(msg_attr[NBD_ATTR_INDEX]);
    cfg->devpath = "/dev/nbd" + std::to_string(index);

    return NL_OK;
}

int NetLinkController::ConnectInternal(NBDConfig* config,
                                       int sockfd,
                                       uint64_t size,
                                       uint32_t blocksize,
                                       uint64_t flags) {
    struct nlattr *sock_attr = nullptr;
    struct nlattr *sock_opt = nullptr;
    struct nl_msg *msg = nullptr;
    int ret;

    nl_socket_modify_cb(sock_, NL_CB_VALID, NL_CB_CUSTOM,
                        netlink_connect_cb, config);
    msg = nlmsg_alloc();
    if (msg == nullptr) {
        dout << "curve-nbd: Could not allocate netlink message." << std::endl;
        return -ENOMEM;
    }

    auto user_hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ,
                                nlId_, 0, 0, NBD_CMD_CONNECT, 0);
    if (user_hdr == nullptr) {
        dout << "curve-nbd: Could not setup message." << std::endl;
        goto nla_put_failure;
    }

    if (!config->devpath.empty()) {
        int index = parse_nbd_index(config->devpath);
        if (index < 0) {
            goto nla_put_failure;
        }
        NLA_PUT_U32(msg, NBD_ATTR_INDEX, index);
    }
    if (config->timeout >= 0) {
        NLA_PUT_U64(msg, NBD_ATTR_TIMEOUT, config->timeout);
    }
    NLA_PUT_U64(msg, NBD_ATTR_SIZE_BYTES, size);
    NLA_PUT_U64(msg, NBD_ATTR_BLOCK_SIZE_BYTES, blocksize);
    NLA_PUT_U64(msg, NBD_ATTR_SERVER_FLAGS, flags);

    sock_attr = nla_nest_start(msg, NBD_ATTR_SOCKETS);
    if (sock_attr == nullptr) {
        dout << "curve-nbd: Could not init sockets in netlink message."
             << std::endl;
        goto nla_put_failure;
    }

    sock_opt = nla_nest_start(msg, NBD_SOCK_ITEM);
    if (sock_opt == nullptr) {
        dout << "curve-nbd: Could not init sock in netlink message."
             << std::endl;
        goto nla_put_failure;
    }

    NLA_PUT_U32(msg, NBD_SOCK_FD, sockfd);
    nla_nest_end(msg, sock_opt);
    nla_nest_end(msg, sock_attr);

    ret = nl_send_sync(sock_, msg);
    if (ret < 0) {
        dout << "curve-nbd: netlink connect failed: " << nl_geterror(ret)
             << std::endl;
        return -EIO;
    }
    return 0;

nla_put_failure:
    nlmsg_free(msg);
    return -EIO;
}

int NetLinkController::DisconnectInternal(int index) {
    struct nl_msg *msg = nullptr;
    int ret;

    nl_socket_modify_cb(sock_, NL_CB_VALID, NL_CB_CUSTOM,
                        genl_handle_msg, NULL);
    msg = nlmsg_alloc();
    if (msg == nullptr) {
        dout << "curve-nbd: Could not allocate netlink message." << std::endl;
        return -EIO;
    }

    auto user_hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ,
                                nlId_, 0, 0, NBD_CMD_DISCONNECT, 0);
    if (user_hdr == nullptr) {
        dout << "curve-nbd: Could not setup message." << std::endl;
        goto nla_put_failure;
    }

    NLA_PUT_U32(msg, NBD_ATTR_INDEX, index);

    ret = nl_send_sync(sock_, msg);
    if (ret < 0) {
        dout << "curve-nbd: netlink disconnect failed: "
             << nl_geterror(ret) << std::endl;
        return -EIO;
    }

    return 0;

nla_put_failure:
    nlmsg_free(msg);
    return -EIO;
}

int NetLinkController::ResizeInternal(int nbdIndex, uint64_t size) {
    struct nl_msg *msg = nullptr;
    int ret;

    nl_socket_modify_cb(sock_, NL_CB_VALID, NL_CB_CUSTOM,
                        genl_handle_msg, NULL);
    msg = nlmsg_alloc();
    if (msg == nullptr) {
        dout << "curve-nbd: Could not allocate netlink message." << std::endl;
        return -EIO;
    }

    auto user_hdr = genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ,
                                nlId_, 0, 0, NBD_CMD_RECONFIGURE, 0);
    if (user_hdr == nullptr) {
        dout << "curve-nbd: Could not setup message." << std::endl;
        goto nla_put_failure;
    }

    NLA_PUT_U32(msg, NBD_ATTR_INDEX, nbdIndex);
    NLA_PUT_U64(msg, NBD_ATTR_SIZE_BYTES, size);

    ret = nl_send_sync(sock_, msg);
    if (ret < 0) {
        dout << "curve-nbd: netlink resize failed: "
             << nl_geterror(ret) << std::endl;
        return -EIO;
    }

    return 0;

nla_put_failure:
    nlmsg_free(msg);
    return -EIO;
}

}  // namespace nbd
}  // namespace curve

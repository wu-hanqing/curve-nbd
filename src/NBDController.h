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

#ifndef NBD_SRC_NBDCONTROLLER_H_
#define NBD_SRC_NBDCONTROLLER_H_

#include <linux/nbd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
// 需要安装 libnl-3-dev libnl-genl-3-dev package
#include <libnl3/netlink/genl/genl.h>
#include <libnl3/netlink/genl/ctrl.h>
#include <libnl3/netlink/genl/mngt.h>
#include <string>
#include <memory>

#include "nbd/src/nbd-netlink.h"
#include "nbd/src/define.h"
#include "nbd/src/util.h"

namespace curve {
namespace nbd {

// 控制NBD内核模块，包括与内核模块通信连接的建立和断开
class NBDController {
 public:
    NBDController() : nbdFd_(-1), nbdIndex_(-1) {}
    virtual ~NBDController() {
        ClearUp();
    }

    /**
     * @brief: 安装NBD设备，并初始化设备属性
     * @param config: 启动NBD设备相关的配置参数
     * @param sockfd: socketpair其中一端的fd，传给NBD设备用于跟NBDServer间的数据传输
     * @param size: 设置NBD设备的大小
     * @param blocksize: device's block size
     * @param flags: 设置加载NBD设备的flags
     * @return: 成功返回0，失败返回负值
     */
    virtual int SetUp(NBDConfig* config,
                      int sockfd,
                      uint64_t size,
                      uint32_t blocksize,
                      uint64_t flags) = 0;
    /**
     * @brief: 根据设备名来卸载已经映射的NBD设备
     * @param devpath: 设备路径，例如/dev/nbd0
     * @return: 成功返回0，失败返回负值
     */
    virtual int DisconnectByPath(const std::string& devpath) = 0;
    /**
     * @brief: 重新设置nbd设备对外显示的大小，可通过lsblk查看
     * @param size: 需要更新的设备的大小
     * @return: 成功返回0，失败返回负值
     */
    virtual int Resize(uint64_t size) = 0;

    // 如果nbd设备已经加载，这里会阻塞，直到NBD出现异常或者收到disconnect命令
    virtual void RunUntilQuit() {
        if (nbdFd_ < 0) {
            return;
        }
        ioctl(nbdFd_, NBD_DO_IT);
    }

    // 清理释放资源
    void ClearUp() {
        if (nbdFd_ < 0) {
            return;
        }
        close(nbdFd_);
        nbdFd_ = -1;
        nbdIndex_ = -1;
    }

    // 获取NBD设备的index，该值就是设备路径/dev/nbd{num}中的num
    int GetNBDIndex() {
        return nbdIndex_;
    }

    // 用来获取当前的Controller是不是netlink形式与内核通信
    virtual bool IsNetLink() {
        return false;
    }

    // 如果flag中带NBD_FLAG_READ_ONLY，将nbd设备设置为只读模式
    int CheckSetReadOnly(int nbdfd, int flag) {
        int arg = 0;
        if (flag & NBD_FLAG_READ_ONLY) {
            arg = 1;
        }
        int ret = ioctl(nbdfd, BLKROSET, (unsigned long) &arg);  // NOLINT
        if (ret < 0) {
            ret = -errno;
        }
        return ret;
    }

 protected:
    int nbdFd_;
    int nbdIndex_;
};
using NBDControllerPtr = std::shared_ptr<NBDController>;

class IOController : public NBDController {
 public:
    IOController() {}
    ~IOController() {}

    int SetUp(NBDConfig* config,
              int sockfd,
              uint64_t size,
              uint32_t blocksize,
              uint64_t flags) override;
    int DisconnectByPath(const std::string& devpath) override;
    int Resize(uint64_t size) override;

 private:
    int InitDevAttr(NBDConfig* config,
                    uint64_t size,
                    uint32_t blocksize,
                    uint64_t flags);
    int MapOnUnusedNbdDevice(int sockfd, std::string* devpath);
    int MapOnNbdDeviceByDevPath(int sockfd, const std::string& devpath,
                                bool logWhenError = true);
};

class NetLinkController : public NBDController {
 public:
    NetLinkController() : nlId_(-1), sock_(nullptr) {}
    ~NetLinkController() {}

    int SetUp(NBDConfig* config,
              int sockfd,
              uint64_t size,
              uint32_t blocksize,
              uint64_t flags) override;
    int DisconnectByPath(const std::string& devpath) override;
    int Resize(uint64_t size) override;
    bool Support();

    bool IsNetLink() override {
        return true;
    }

 private:
    int Init();
    void Uninit();
    int ConnectInternal(NBDConfig* config,
                        int sockfd,
                        uint64_t size,
                        uint32_t blocksize,
                        uint64_t flags);
    int DisconnectInternal(int index);
    int ResizeInternal(int nbdIndex, uint64_t size);

 private:
    int nlId_;
    struct nl_sock* sock_;
};

}  // namespace nbd
}  // namespace curve

#endif  // NBD_SRC_NBDCONTROLLER_H_

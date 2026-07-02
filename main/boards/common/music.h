#ifndef MUSIC_H
#define MUSIC_H

#include <string>

class Music {
public:
    virtual ~Music() = default;

    // 开始播放歌曲（阻塞式搜索+启动播放任务）
    // 在 Application::Schedule 回调中调用，确保已断开服务器
    virtual bool Start(const std::string& song_name, const std::string& artist_name = "") { return false; }

    // 停止播放（线程安全，可从任何上下文调用）
    virtual void Stop() {}

    // 暂停（保存进度，可通过 Resume 续播）
    virtual void Pause() { Stop(); }

    // 续播上次暂停的音乐（需先调用过 Pause）
    virtual void Resume() {}

    // 是否可续播
    virtual bool CanResume() const { return false; }

    // 是否正在播放
    virtual bool IsPlaying() const { return false; }

    // 搜索歌曲（不播放），返回 JSON 格式的搜索结果
    virtual std::string Search(const std::string& song_name, const std::string& artist_name = "") { return "{}"; }

    // 获取当前播放信息
    virtual std::string GetSongName() const { return ""; }
    virtual std::string GetArtistName() const { return ""; }
};

// 工厂函数：创建音乐播放器实例（需要 I2S 输出就绪后调用）
Music* CreateMusic();

#endif // MUSIC_H

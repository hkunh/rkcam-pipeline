#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>


namespace rkcam{

enum class QueueFullPolicy{
    Block,       // 队列满时阻塞等待
    DropNewest,  // 队列满时丢弃当前新帧
    DropOldest,  // 队列满时丢弃最旧帧，插入新帧
};
template <typename T>
class BlockingQueue{
public:
    explicit BlockingQueue(
        size_t capacity = 8,
        QueueFullPolicy full_policy = QueueFullPolicy::DropOldest) 
        : capacity_(capacity), 
        full_policy_(full_policy)
    {
        if(capacity_ == 0){
            capacity_ = 1;
        }
    }


    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    bool push(T item)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if(stopped_)
        {
            return false;
        }
        if(full_policy_ == QueueFullPolicy::Block){
            not_full_cv_.wait(lock, [this](){
                return stopped_ || queue_.size() < capacity_;
            });
            if(stopped)
            {
                return false;
            }
            queue_.push_back(std::move(item));
        }
        else if(full_policy_ == QueueFullPolicy::DropNewest){
            if(queue_.size() >= capacity_)
            {
                ++dropped_count_;
                return false;
            }
            queue_.push_back(std::move(item));
        }
        else{
            if(queue_.size() >= capacity_)
            {
                queue_.pop_front();
                ++dropped_count_;
            }
            queue_.push_back(std::move(item));
        }
        not_empty_cv_.notify_one();
        return true;
    }
    bool pop(T& item)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        not_empty_cv_.wait(lock, [this](){
            return stopped_ || !queue_.empty();
        });

        if(queue_.empty())
        {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop_front();

        not_full_cv_.notify_one();
        return true;
    }

    bool tryPop(T& item){
        std::lock_guard<std::mutex> lock(mutex_);

        if(queue_.empty())
        {
            return false;
        }
        item = std::move(queue_.front());
        not_full_cv_.notify_one();
        return true;
    }


    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }
    void reset() //相当于start
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = false;
            queue_.clear();
            dropped_count_ = 0;
        }
        not_full_cv_.notify_all();
    }
    void clear()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        not_full_cv_.notify_all();
    }
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t capacity() const
    {
        return capacity_;
    }

    size_t droppedCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_count_;
    }

    bool stopped() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }
private:
    size_t capacity_ = 8;
    QueueFullPolicy full_policy_ = QueueFullPolicy::DropOldest;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;
    std::condition_variable not_full_cv_;

    std::deque<T> queue_;

    bool stopped_ = false;
    size_t dropped_count_ = 0;
};

}
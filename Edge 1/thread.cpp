#include<bits/stdc++.h>
using namespace std;

class Thread{

private:
    thread t;

public:
    template<typename Func,typename... Args>
    explicit Thread(Func&& f,Args&&... args){
        try{
            t=thread(forward<Func>(f),forward<Args>(args)...);
        }
        catch(const system_error&e){
            cerr<<"Thread creation failed: "<<e.what()<<endl;
            throw;
        }
    }

    ~Thread(){
        if(t.joinable()){
            t.join();
        }
    }

    Thread(const Thread&)=delete;
    Thread& operator=(const Thread&)=delete;

    // 这里的移动函数只能借助系统提供的函数实现，因为我们没有权限
    Thread(Thread&& other)noexcept:t(move(other.t)){}

    Thread& operator=(Thread&& other)noexcept{
        if(this!=&other){
            if(t.joinable()){
                t.join();
                t=move(other.t);
            }
            return *this;
        }
    }
};

void worker(int id,int sec){
    try{
        cout<<"Thread "<<id<<" work"<<endl;
        this_thread::sleep_for(chrono::seconds(sec));
        if(id%2==0){
            throw runtime_error("Thread "+to_string(id)+" failed");
        }
        cout<<"Thread "<<id<<" end"<<endl;
    }
    catch(const exception& e){
        cout<<"Thread "<<id<<" : "<<e.what()<<endl;
    }
}

void fun(string str){
    cout<<"hello world! "<<"hello "<<str<<endl;
}

int  main()
{
    try{
        Thread t1(worker,1,2);
        Thread t2(worker,2,3);
        Thread t3(fun,"wuyuhui");

        Thread t4=move(t1);

        {
            Thread t5(worker,4,1);
        }
    }
    catch(const exception& e){
        cerr<<"failed : "<<e.what()<<endl;
    }
    return 0;
}
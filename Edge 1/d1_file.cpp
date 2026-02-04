#include<bits/stdc++.h>
using namespace std;

class File{

private:
    FILE* fptr;

public:
    File(const string& filename,const string& mode):fptr(nullptr){
        fptr=fopen(filename.c_str(),mode.c_str());
        if(!fptr){
            cerr<<"Failed to open file : "<<filename<<endl;
            fptr=nullptr;
            // 即使构造失败，也不直接exit
        }
    }

    // 禁止拷贝，无论是深拷贝还是浅拷贝
    File(const File&)=delete;
    File& operator=(const File&)=delete;

    // 支持移动
    File(File&& other) noexcept:fptr(other.fptr){
        cout<<"Move Constructor"<<endl;
        other.fptr=nullptr;
    }

    File& operator=(File&& other) noexcept{
        cout<<"Move Assignment"<<endl;
        if(this!=&other){
            close();
            fptr=other.fptr;
            other.fptr=nullptr;
        }
        return *this;
    }

    ~File(){
        close();
    }

    bool is_valid()const{
        return fptr!=nullptr;
    }

    bool write(const string& data){
        if(!fptr){
            return false;
        }
        return fwrite(data.c_str(),1,data.size(),fptr)==data.size();
    }

    string read_line(){
        if(!fptr){
            return "";
        }
        char buffer[1024];
        if(fgets(buffer,sizeof(buffer),fptr)){
            return string(buffer);
        }
        return "";
    }

    void seek_to_begin(){
        if(fptr){
            fseek(fptr,0,SEEK_SET);
        }
    }

    void flush(){
        if(fptr){
            fflush(fptr);
        }
    }

private:
    void close(){
        if(fptr){
            cout<<"file close"<<endl;
            fclose(fptr);
            fptr=nullptr;
        }
    }

};

int main()
{
    File f("test.txt","w+");
    if(!f.is_valid()){
        cout<<"cannot open file"<<endl;
        return 1;
    }
    else{
        cout<<"can open file"<<endl;
    }

    f.write("hello world!\n");
    f.flush();

    File f1=move(f);
    f1.write("now,this is f1\n");

    cout<<f.is_valid()<<endl;

    f=move(f1);
    cout<<f.is_valid()<<endl;

    f.seek_to_begin();
    cout<<f.read_line();
    cout<<f.read_line();
    return 0;
}
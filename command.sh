sudo apt-get update 
sudo apt-get upgrade
sudo apt-get install binutils g++ cmake
sudo apt-get install mysql-server nginx supervisor
sudo apt-get install unixodbc unixodbc-dev libssl-dev
#download and extract Boost C++
wget "https://boostorg.jfrog.io/artifactory/main/release/1.76.0/source/boost_1_76_0.tar.gz"
tar -xzvf boost_1_76_0.tar.gz*

#export Boost
export BOOST_ROOT=/root/boost_1_76_0/

# compile the program
cmake . && make

# unzip and enter the server files
tar -xzvf server_files.tar.gz && cd server_files/

#copy the ODBC files for MySQL into the directory
sudo cp *.so /usr/lib/x86_64-linux-gnu/odbc/

# copy the ODBC configuration files into /etc/
sudo cp *.ini /etc/

sudo cp supervisord.conf /etc/supervisor/
sudo cp nginx.conf /etc/nginx/

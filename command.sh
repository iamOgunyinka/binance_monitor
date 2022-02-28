sudo apt-get update 
sudo apt-get upgrade
sudo apt-get install binutils g++ cmake
sudo apt-get install mysql-server nginx supervisor
sudo apt-get install unixodbc unixodbc-dev libssl-dev libz-dev
#download and extract Boost C++
wget "https://boostorg.jfrog.io/artifactory/main/release/1.76.0/source/boost_1_76_0.tar.gz"
tar -xzvf boost_1_76_0.tar.gz*

#export Boost
CURR_WD=$PWD
export BOOST_ROOT=$CURR_WD/boost_1_76_0/

#clone git and cd into it...
git clone "https://github.com/iamOgunyinka/binance_monitor.git"

cd $PWD/binance_monitor/

# unzip and enter the server files
mkdir server_files
tar -xzvf server_files.tar.gz -C server_files/
cd server_files/

#copy the ODBC files for MySQL into the directory
sudo cp *.so /usr/lib/x86_64-linux-gnu/odbc/

# copy the ODBC configuration files into /etc/
sudo cp *.ini /etc/
sudo cp supervisord.conf /etc/supervisor/
sudo cp nginx.conf /etc/nginx/

cd ../binance_orders/
git submodule update --init --recursive

# cd into each program and compile them
cmake . && make

cd ../binance_prices/
git submodule update --init --recursive
cmake . && make

cd $CURR_WD/binance_monitors/
mkdir log
mkdir config
mkdir log/prices log/orders
touch config/info.json

service nginx start

echo =======================================================
echo "You'll need to edit the /etc/supervisor/supervisor.conf"
echo file to point to the location of the new executables and
echo "then run 'service supervisor start' to run the servers."
echo =======================================================

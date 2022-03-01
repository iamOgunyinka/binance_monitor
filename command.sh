sudo apt-get update 
sudo apt-get upgrade
sudo apt-get install binutils g++ cmake
sudo apt-get install mysql-server nginx supervisor
sudo apt-get install unixodbc unixodbc-dev libssl-dev libz-dev
#download and extract Boost C++
wget "https://boostorg.jfrog.io/artifactory/main/release/1.76.0/source/boost_1_76_0.tar.gz"
tar -xzvf boost_1_76_0.tar.gz*

#export Boost
export BOOST_ROOT=$PWD/boost_1_76_0/

#clone git and cd into it...
git clone "https://github.com/iamOgunyinka/binance_monitor.git"
ROOT_DIR=$PWD/binance_monitor

cd $ROOT_DIR

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

cd $ROOT_DIR/binance_orders/
git submodule update --init --recursive

# cd into each program and compile them
cmake . && make

cd $ROOT_DIR/binance_prices/
git submodule update --init --recursive
cmake . && make

cd $ROOT_DIR
mkdir log
mkdir log/prices log/orders

echo "*******************************************************"
echo "The 'config/info.json' file is just a sample config"
echo file and you will need to edit it for more flexibility.
echo "*******************************************************"

# use a sample configuration file
mkdir config
mv $ROOT_DIR/sample_config.json $ROOT_DIR/config/info.json

# start the reverse proxy server
service nginx start

echo =======================================================
echo If starting supervisor fails, you may need to edit the
echo "'/etc/supervisor/supervisor.conf' file to point to the"
echo location of the new executables and then run
echo "'service supervisor start' to run the services."
echo =======================================================

service supervisor start

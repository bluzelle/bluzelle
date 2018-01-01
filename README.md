BUILD INSTRUCTIONS
==================

macOS BOOST Installation 
-
Bluzelle daemon uses the boost libraries, notably the Beast Library by Vinnie Falco, so we require Boost version 1.66.0, 
released on December 18th, 2017.    

NOTE: This will overwrite an existing Boost installation if it exists, if you have installed Boost with 
brew or some other package manager, you may wish to uninstall the older version of Boost first.

So open up a console and get started by downloading boost and building:
```
wget -c http://sourceforge.net/projects/boost/files/boost/1.66.0/boost_1_66_0.tar.bz2/download
tar -xzvf download
rm download
cd boost_1_66_0
./bootstrap.sh 
./b2 toolset=darwin install
```

Feel free to use the "-j" option with b2 to speed things up a bit. 

Now you will have the Boost 1.66.0 libraries installed in 

```/usr/local/lib```

and the include files in 

```/usr/local/include/boost```


*nix BOOST Installation
-
```
wget -c http://sourceforge.net/projects/boost/files/boost/1.66.0/boost_1_66_0.tar.bz2
tar jxf boost_1_66_0.tar.bz2
cd boost_1_66_0
sudo ./bootstrap.sh --prefix=/usr/local/
./b2
sudo ./b2 install 
```
If you are getting errors that some Python headers are missing you need to install python-dev with and re-run b2 again
```
apt-get install python-dev
```

Windows BOOST Installation
-
TBD

CMake Installation
-
If you dont have CMake version 3.10 or above you have to install it. Please download it from https://cmake.org/download/
On Linux you can build CMake with (use -j option to build on multiple CPUs)
```
./bootstrap
make
sudo make install
```

Alternatively 
```
wget https://cmake.org/files/v3.10/cmake-3.10.1.tar.gz
tar -xzf cmake-3.10.1.tar.gz
cd cmake-3.10.1
./configure
make
sudo make install
```

Pre-built binaries also available for MacOS and Windows

NPM and WSCAT installation
-
Install npm if you don't have it with
```
sudo npm install npm@latest -g
```
Install wscat 
```
sudo npm install wscat -g
```
WSCAT installation without NPM
```
sudo apt-get install node-ws
```

CLONING THE REPO (All OS's)
-
We keep the project on github, so just clone the repo in a convenient folder:

git clone https://github.com/bluzelle/bluzelle.git

BUILDING THE DAEMON
-
If you have cLion, open the folder that you have just cloned otherwise you can build from the command line. 
Here are the steps to build the daemon and unit test application from the command line:
```
git checkout devel
cd bluzelle
mkdir build
cd build
cmake ..
make
````

The executables ```the_db``` and ```the_db_test```  will be in the `daemon` folder.


SETTING ENVIRONMENT
-
- Go to https://etherscan.io/login?cmd=last and login (create an account if necessary). Click API-KEYs and then Create API Key and create a new token, this token will be used to make calls to Ethereum Ropsten network. Naming the token is optinal.
- Create an environment variable named ETHERSCAN_IO_API_TOKEN with the newly created key as value (there are different ways to do that depending on system you are using). You can add it to your shell profile. Alternatively you can prepend the application you about to run with environment variable i.e.
```
ETHERSCAN_IO_API_TOKEN=TOKEN_GOES_HERE ./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58001
```
- The API token is used to make a calls to the Ethereum network. In order to run a node the farmer is supposed to have at least 100 tokens in his/her account. The account address is provided as a parameter on command line. For the purpose of this demo we use the address 0x006eae72077449caca91078ef78552c0cd9bce8f that has the required amount. Currently the_db checks for the MK_13 token balance.

RUNNING THE APPLICATION
-
- Change to the directory where the_db file is located and open 6 terminal sessions (or tabs). Currently the_db supports only 5 follower nodes. The follower nodes are expected on the same machine on ports following the leader (i.e if leader runs on port 58000, the_db expects followers on ports 58001 to 58005 inclusive on the same machine). Port must be in range 49152 - 65535.
- In first 5 tabs run the_db in "follower" mode i.e. using port number that does not start with 0. for example:
```
./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58001
./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58002
./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58003
./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58004
./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58005
```
- Then in remaining tab run the_db in leader mode i.e. with port ending with 0. The leader will start sending heartbeats to followers.
```
./the_db --address 0x006eae72077449caca91078ef78552c0cd9bce8f --port 58000
```
- Open _another_ terminal window. This window will emulate API. Type 
```
wscat -c localhost:58000
```
It will create websocket connection to the leader node (running on port 58000)
- Paste following JSON to wscat and press Enter:
```
{"bzn-api":"create", "transaction-id":"123", "data":{"key":"key_222", "value":"value_222"}}
```
It will send command to leader to create an entry in database with key ```key_222``` and value ```value_222``` (feel free to use any key/values). Transaction ID is arbitrary for now.
- Press Ctrl-C to disconnect from leader node.
Run wscat again and connect to one of the followers nodes (running on ports 58001 to 58005).
```
wscat -c localhost:58004
```
- Paste following JSON and press Enter.
```
{"bzn-api":"read", "transaction-id":"123", "data":{"key":"key_222"}}
```
This will send request to the node to retrieve value for key ```key_222```. You should get JSON back with result ```value_222```.
- You can disconnect and connect to another follower node and send same JSON, correct value will be returned.

This means that data was successfully propagated from leader to followers nodes.


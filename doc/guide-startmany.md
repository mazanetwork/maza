# start-many Setup Guide

## Setting up your Wallet

### Create New Wallet Addresses

1. Open the QT Wallet.
2. Click the Receive tab.
3. Fill in the form to request a payment.
    * Label: mn01
    * Amount: 1000 (optional)
    * Click *Request payment* button
5. Click the *Copy Address* button

Create a new wallet address for each Mazanode.

Close your QT Wallet.

### Send 1000 MAZA to New Addresses

Send exactly 1000 MAZA to each new address created above.

### Create New Mazanode Private Keys

Open your QT Wallet and go to console (from the menu select `Tools` => `Debug Console`)

Issue the following:

```mazanode genkey```

*Note: A mazanode private key will need to be created for each Mazanode you run. You should not use the same mazanode private key for multiple Mazanodes.*

Close your QT Wallet.

## <a name="mazanodeconf"></a>Create mazanode.conf file

Remember... this is local. Make sure your QT is not running.

Create the `mazanode.conf` file in the same directory as your `wallet.dat`.

Copy the mazanode private key and correspondig collateral output transaction that holds the 1000 MAZA.

*Note: The mazanode priviate key is **not** the same as a wallet private key. **Never** put your wallet private key in the mazanode.conf file. That is almost equivalent to putting your 1000 MAZA on the remote server and defeats the purpose of a hot/cold setup.*

### Get the collateral output

Open your QT Wallet and go to console (from the menu select `Tools` => `Debug Console`)

Issue the following:

```mazanode outputs```

Make note of the hash (which is your collateral_output) and index.

### Enter your Mazanode details into your mazanode.conf file
[From the maza github repo](https://github.com/mazacoin/maza/blob/master/doc/mazanode_conf.md)

`mazanode.conf` format is a space seperated text file. Each line consisting of an alias, IP address followed by port, mazanode private key, collateral output transaction id and collateral output index.

```
alias ipaddress:port mazanode_private_key collateral_output collateral_output_index
```

Example:

```
mn01 127.0.0.1:9999 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0
mn02 127.0.0.2:9999 93WaAb3htPJEV8E9aQcN23Jt97bPex7YvWfgMDTUdWJvzmrMqey aa9f1034d973377a5e733272c3d0eced1de22555ad45d6b24abadff8087948d4 0
```

## Update maza.conf on server

If you generated a new mazanode private key, you will need to update the remote `maza.conf` files.

Shut down the daemon and then edit the file.

```nano .mazanetwork/maza.conf```

### Edit the mazanodeprivkey
If you generated a new mazanode private key, you will need to update the `mazanodeprivkey` value in your remote `maza.conf` file.

## Start your Mazanodes

### Remote

If your remote server is not running, start your remote daemon as you normally would. 

You can confirm that remote server is on the correct block by issuing

```maza-cli getinfo```

and comparing with the official explorer at https://explorer.mazacoin.org/chain/Maza

### Local

Finally... time to start from local.

#### Open up your QT Wallet

From the menu select `Tools` => `Debug Console`

If you want to review your `mazanode.conf` setting before starting Mazanodes, issue the following in the Debug Console:

```mazanode list-conf```

Give it the eye-ball test. If satisfied, you can start your Mazanodes one of two ways.

1. `mazanode start-alias [alias_from_mazanode.conf]`  
Example ```mazanode start-alias mn01```
2. `mazanode start-many`

## Verify that Mazanodes actually started

### Remote

Issue command `mazanode status`
It should return you something like that:
```
maza-cli mazanode status
{
    "outpoint" : "<collateral_output>-<collateral_output_index>",
    "service" : "<ipaddress>:<port>",
    "pubkey" : "<1000 MAZA address>",
    "status" : "Mazanode successfully started"
}
```
Command output should have "_Mazanode successfully started_" in its `status` field now. If it says "_not capable_" instead, you should check your config again.

### Local

Search your Mazanodes on https://mazaninja.pl/mazanodes.html

_Hint: Bookmark it, you definitely will be using this site a lot._

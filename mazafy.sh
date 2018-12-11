#!/bin/bash  -x

test -f src/qt/dash.cpp || { echo "Your current directory is $(pwd)" ;echo  "You should be in the toplevel directory of the project to mazafy it" ; exit 1; }

if [ "$0" != "/tmp/mazafy/mazafy.sh" ] ; then
   mkdir /tmp/mazafy || { echo "Could not create /tmp/mazafy" ; exit 1; }
   cp ./mazafy.sh /tmp/mazafy || { echo "Could not move script to /tmp/mazafy" ; exit 1; }
   exec /tmp/mazafy/mazafy.sh
fi

find . -type d |egrep -v '\./\.git' > /tmp/mazafy/mazafy.dirs
while read line ; do
  pushd $line
  find ./ -type f -not -name '*.png' -not -name '*.raw' -not -name '*.ico' -not -name '*.bmp' -not -name mazafy.sh -not -name '*.icns'  -maxdepth 1 > /tmp/mazafy/filelist
  while read filename ; do
    echo "change $(pwd)/$filename"
    sed -i.bkp -e 's/[Dd]ash\ [Cc]ore/Maza\ Network/g' -e 's/dashcore/mazanetwork/g' -e 's/dashpay/mazacoin/g' -e 's/Dash/Maza/g' -e 's/DASH/MAZA/g' -e 's/dash/maza/g' $filename
    sed -i.bkp -e 's/MasterNode/MazaNode/g' -e 's/masternode/mazanode/g' -e 's/Masternode/Mazanode/g' -e 's/MASTERNODE/MAZANODE/g' $filename
  done < /tmp/mazafy/filelist
  rm /tmp/mazafy/filelist
  popd
done < /tmp/mazafy/mazafy.dirs
cat << EOF > /tmp/mazafy/mazafy.files
./contrib/init/dashd.conf
./contrib/init/dashd.init
./contrib/init/dashd.openrc
./contrib/init/dashd.openrcconf
./contrib/init/dashd.service
./contrib/init/org.dash.dashd.plist
./contrib/dash-cli.bash-completion
./contrib/dash-qt.pro
./contrib/dash-tx.bash-completion
./contrib/dashd.bash-completion
./doc/man/dash-cli.1
./doc/man/dash-qt.1
./doc/man/dash-tx.1
./doc/man/dashd.1
./doc/dashcoin-developer-notes.md
./libdashconsensus.pc.in
./src/bench/bench_dash.cpp
./src/dash-cli-res.rc
./src/dash-cli.cpp
./src/dash-tx-res.rc
./src/dash-tx.cpp
./src/dashd-res.rc
./src/dashd.cpp
./src/qt/locale/dash_bg.ts
./src/qt/locale/dash_de.ts
./src/qt/locale/dash_en.ts
./src/qt/locale/dash_es.ts
./src/qt/locale/dash_fi.ts
./src/qt/locale/dash_fr.ts
./src/qt/locale/dash_it.ts
./src/qt/locale/dash_ja.ts
./src/qt/locale/dash_nl.ts
./src/qt/locale/dash_pl.ts
./src/qt/locale/dash_pt.ts
./src/qt/locale/dash_pt_BR.ts
./src/qt/locale/dash_ru.ts
./src/qt/locale/dash_sk.ts
./src/qt/locale/dash_sv.ts
./src/qt/locale/dash_vi.ts
./src/qt/locale/dash_zh_CN.ts
./src/qt/locale/dash_zh_TW.ts
./src/qt/dash.cpp
./src/qt/dash.qrc
./src/qt/dash_locale.qrc
./src/qt/dashstrings.cpp
./src/qt/res/images/crownium/dash_logo_horizontal.png
./src/qt/res/images/drkblue/dash_logo_horizontal.png
./src/qt/res/images/light/dash_logo_horizontal.png
./src/qt/res/images/light-retro/dash_logo_horizontal.png
./src/qt/res/images/trad/dash_logo_horizontal.png
./src/qt/res/dash-qt-res.rc
./src/script/dashconsensus.cpp
./src/script/dashconsensus.h
./src/test/test_dash.cpp
./src/test/test_dash.h
EOF
while read line ; do 
  fname=$(echo $line | sed 's/dash/maza/g')
  mv $line $fname 
done < /tmp/mazafy/mazafy.files

cat << EOF > /tmp/mazafy/de_masternode.files
./doc/masternode-budget.md
./doc/masternode_conf.md
./src/activemasternode.cpp
./src/activemasternode.h
./src/masternode-payments.cpp
./src/masternode-payments.h
./src/masternode-sync.cpp
./src/masternode-sync.h
./src/masternode.cpp
./src/masternode.h
./src/masternodeconfig.cpp
./src/masternodeconfig.h
./src/masternodeman.cpp
./src/masternodeman.h
./src/qt/forms/masternodelist.ui
./src/qt/masternodelist.cpp
./src/qt/masternodelist.h
./src/qt/res/icons/crownium/masternodes.png
./src/qt/res/icons/drkblue/masternodes.png
./src/qt/res/icons/light/masternodes.png
./src/qt/res/icons/light-retro/masternodes.png
./src/qt/res/icons/trad/masternodes.png
./src/rpc/masternode.cpp
EOF
while read line ; do 
  fname=$(echo $line | sed 's/masternode/mazanode/g')
  mv $line $fname 
done < /tmp/mazafy/de_masternode.files

# exceptions 

# .travis.yml
# /bin/dash should not change to /bin/maza
sed -i.bkp -e 's/\/bin\/maza/\/bin\/dash/g' .travis.yml

#diff -r ./src/clientversion.cpp ../XXXDASH_untouched/src/clientversion.cpp
#16c16
#< const std::string CLIENT_NAME("Buffalo");
#---
#> const std::string CLIENT_NAME("Dash Core");
#diff -r ./src/clientversion.h ../XXXDASH_untouched/src/clientversion.h
#41c41
#< #define COPYRIGHT_STR "2009-" STRINGIZE(COPYRIGHT_YEAR) " The Dash Core Developers"
#---
#> #define COPYRIGHT_STR "2009-" STRINGIZE(COPYRIGHT_YEAR) " The Bitcoin Core Developers, 2014-" STRINGIZE(COPYRIGHT_YEAR) " " COPYRIGHT_HOLDERS_FINAL
#
#diff -r ./src/validation.cpp ../XXXDASH_untouched/src/validation.cpp
#106c106
#< const std::string strMessageMagic = "Dash Signed Message:\n";
#---
#> const std::string strMessageMagic = "DarkCoin Signed Message:\n";


# clean up 
# in case we modify above and self-hose our script copy the original back from /tmp
cp /tmp/mazafy/mazafy.sh .
rm -rf /tmp/mazafy
# remove files named with .![PID] - binary files we tried to change with sed
find ./ -name '.\!*' | xargs rm 
# remove sed backups 
find ./ -name '*.bkp' | xargs rm

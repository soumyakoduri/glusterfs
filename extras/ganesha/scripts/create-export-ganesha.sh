#/bin/bash

GANESHA_DIR=$2

function write_conf()
{
echo "EXPORT{"
echo " Export_Id = 1;"
echo "Path=\"/$1\";"
echo "FSAL {"
echo "name = "GLUSTER";"
echo "hostname=\"localhost\";"
echo  "volume=\"$1\";"
echo "}"
echo "Access_type = RW;"
echo "Squash=\"No_root_squash\";"
echo "Pseudo=\"/$1\";"
echo "Protocols = \"3,4\" ;"
echo "Transports = \"UDP,TCP\" ;"
echo "SecType = \"sys\";"
echo "Tag = \"$1\";"
echo "}"
}

write_conf $@ >> $GANESHA_DIR/exports/export.$1.conf

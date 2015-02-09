#!/bin/bash

. /etc/ganesha/ganesha-ha.conf

HA_NUM_SERVERS=0
HA_SERVERS=""

RHEL6_PCS_CNAME_OPTION="--name"

check_cluster_exists()
{
    local name=${1}
    local cluster_name=""

    if [ -e /var/run/corosync.pid ]; then
        cluster_name=$(pcs status | grep "Cluster name:" | cut -d ' ' -f 3)
        if [ ${cluster_name} -a ${cluster_name} = ${name} ]; then
            echo "$name already exists, exiting"
            exit 0
        fi
    fi
}

determine_servers()
{
    local cmd=${1}
    local num_servers=0
    local tmp_ifs=${IFS}
    local ha_servers=""

    if [[ "X${cmd}X" != "XteardownX" ]]; then
        IFS=$','
        for server in ${HA_CLUSTER_NODES} ; do
            num_servers=$(expr ${num_servers} + 1)
        done
        IFS=${tmp_ifs}
        HA_NUM_SERVERS=${num_servers}
        HA_SERVERS="${HA_CLUSTER_NODES//,/ }"
    else
        ha_servers=$(pcs status | grep "Online:" | grep -o '\[.*\]' | sed -e 's/\[//' | sed -e 's/\]//')
        IFS=$' '
        for server in ${ha_servers} ; do
            num_servers=$(expr ${num_servers} + 1)
        done
        IFS=${tmp_ifs}
        HA_NUM_SERVERS=${num_servers}
        HA_SERVERS="${ha_servers}"
    fi
}

setup_cluster()
{
    local name=${1}
    local num_servers=${2}
    local servers=${3}
    local unclean=""

    echo "setting up cluster ${name} with the following ${servers}"

    pcs cluster auth ${servers}
# fedora    pcs cluster setup ${name} ${servers}
# rhel6     pcs cluster setup --name ${name} ${servers}
    pcs cluster setup ${RHEL6_PCS_CNAME_OPTION} ${name} ${servers}
    if [ $? -ne 0 ]; then
        echo "pcs cluster setup ${RHEL6_PCS_CNAME_OPTION} ${name} ${servers} failed"
        exit 1;
    fi
    pcs cluster start --all
    if [ $? -ne 0 ]; then
        echo "pcs cluster start failed"
        exit 1;
    fi

    echo "waiting for cluster to stabilize..."
    sleep 3
    unclean=$(pcs status | grep -u "UNCLEAN")
    while [[ "${unclean}X" = "UNCLEANX" ]]; do
         sleep 1
         echo -n "."
         unclean=$(pcs status | grep -u "UNCLEAN")
    done
    sleep 1
    echo "...continuing"

    if [ ${num_servers} -lt 3 ]; then
        pcs property set no-quorum-policy=ignore
        if [ $? -ne 0 ]; then
            echo "warning: pcs property set no-quorum-policy=ignore failed"
        fi
    fi
    pcs property set stonith-enabled=false
    if [ $? -ne 0 ]; then
        echo "warning: pcs property set stonith-enabled=false failed"
    fi
}

setup_finalize()
{
    local cibfile=${1}
    local stopped=""

    echo "waiting for resources to start..."
    stopped=$(pcs status | grep -u "Stopped")
    while [[ "${stopped}X" = "StoppedX" ]]; do
         sleep 1
         echo -n "."
         stopped=$(pcs status | grep -u "Stopped")
    done

    pcs status | grep dead_ip-1 | sort > /var/run/ganesha/pcs_status

    echo "...done"

}

teardown_cluster()
{
    local name=${1}
    local remove=""

    echo "tearing down cluster $name"

    for server in ${HA_SERVERS} ; do
        if [[ ${HA_CLUSTER_NODES} != *${server}* ]]; then
            echo "info: ${server} is not in config, removing"

            echo "pcs cluster stop ${server}"
            pcs cluster stop ${server}
            if [ $? -ne 0 ]; then
                pcs cluster stop ${server}
            fi

            echo "pcs cluster node remove ${server}"
            pcs cluster node remove ${server}
            if [ $? -ne 0 ]; then
                echo "warning: pcs cluster node remove ${server} failed"
            fi

            remove="${remove} ${server}"

        fi
    done

    if [[ "X${remove}X" != "XX" ]]; then
	crm_node --list
        sleep 5
        for server in ${remove} ; do
            echo "crm_node -f -R ${server}"
            crm_node -f -R ${server}
            if [ $? -ne 0 ]; then
                echo "warning: crm_node -f -R ${server} failed"
            fi
        done
	crm_node --list
    fi

    echo "pcs cluster stop --all"
    pcs cluster stop --all
    if [ $? -ne 0 ]; then
        echo "warning pcs cluster stop --all failed"
    fi

    pcs cluster destroy
    if [ $? -ne 0 ]; then
        echo "warning pcs cluster destroy failed"
    fi
}

do_create_virt_ip_constraints()
{
    local cibfile=${1}; shift
    local primary=${1}; shift
    local weight="1000"

    # first a constraint location rule that says the VIP must be where
    # there's a ganesha.nfsd running
    echo "pcs -f ${cibfile} constraint location ${primary}-cluster_ip-1 rule score=-INFINITY ganesha-active ne 1"
    pcs -f ${cibfile} constraint location ${primary}-cluster_ip-1 rule score=-INFINITY ganesha-active ne 1
    if [ $? -ne 0 ]; then
        echo "warning: pcs constraint location ${primary}-cluster_ip-1 rule score=-INFINITY ganesha-active ne 1 failed"
    fi

    # then a set of constraint location prefers to set the prefered order
    # for where a VIP should move
    while [[ ${1} ]]; do
        echo "pcs -f ${cibfile} constraint location ${primary}-cluster_ip-1 prefers ${1}=${weight}"
        pcs -f ${cibfile} constraint location ${primary}-cluster_ip-1 prefers ${1}=${weight}
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint location ${primary}-cluster_ip-1 prefers ${1}=${weight} failed"
        fi
        weight=$(expr ${weight} + 1000)
        shift
    done
    # and finally set the highest preference for the VIP to its home node
    # default weight when created is/was 100.
    # on Fedora setting appears to be additive, so to get the desired
    # value we adjust the weight
    # weight=$(expr ${weight} - 100)
    echo "pcs -f ${cibfile} constraint location ${primary}-cluster_ip-1 prefers ${primary}=${weight}"
    pcs -f ${cibfile} constraint location ${primary}-cluster_ip-1 prefers ${primary}=${weight}
    if [ $? -ne 0 ]; then
        echo "warning: pcs constraint location ${primary}-cluster_ip-1 prefers ${primary}=${weight} failed"
    fi
}

wrap_create_virt_ip_constraints()
{
    local cibfile=${1}; shift
    local primary=${1}; shift
    local head=""
    local tail=""

    # build a list of peers, e.g. for a four node cluster, for node1,
    # the result is "node2 node3 node4"; for node2, "node3 node4 node1"
    # and so on.
    echo "primary is ${primary}, first node is ${1}"
    while [[ ${1} ]]; do
        echo "node _${1}_"
        if [ "${1}" = "${primary}" ]; then
            shift
            while [[ ${1} ]]; do
                echo "adding ${1} to tail"
                tail=${tail}" "${1}
                shift
            done
        else
            echo "adding ${1} to head"
            head=${head}" "${1}
        fi
        shift
    done
    echo "calling do_create_virt_ip_constraints ${cibfile} x ${primary} x ${tail} x ${head} x"
    do_create_virt_ip_constraints ${cibfile} ${primary} ${tail} ${head}
}

create_virt_ip_constraints()
{
    local cibfile=${1}; shift
    while [[ ${1} ]]; do
        echo "calling wrap_create with ${1}"
        wrap_create_virt_ip_constraints ${cibfile} ${1} ${HA_SERVERS}
        shift
    done
}


setup_create_resources()
{
    local cibfile=$(mktemp -u)

    # mount the HA-state volume and start ganesha.nfsd on all nodes
    # echo "pcs resource create nfs_start ganesha_nfsd ha_vol_name=${HA_VOL_NAME} ha_vol_mnt=${HA_VOL_MNT} ha_vol_server=${HA_VOL_SERVER} --clone"
    pcs resource create nfs_start ganesha_nfsd ha_vol_name=${HA_VOL_NAME} ha_vol_mnt=${HA_VOL_MNT} ha_vol_server=${HA_VOL_SERVER} --clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource create nfs_start ganesha_nfsd --clone failed"
    fi
    sleep 1
    # cloned resources seem to never have their start() invoked when they
    # are created, but stop() is invoked when they are destroyed. Why???.
    # No matter, we don't want this resource agent hanging around anyway
    # echo "pcs resource delete nfs_start-clone"
    pcs resource delete nfs_start-clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource delete nfs_start-clone failed"
    fi

    # echo "pcs resource create nfs-mon ganesha_mon --clone"
    pcs resource create nfs-mon ganesha_mon --clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource create nfs-mon ganesha_mon --clone failed"
    fi

    # echo "pcs resource create nfs-grace ganesha_grace --clone"
    pcs resource create nfs-grace ganesha_grace --clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource create nfs-grace ganesha_grace --clone failed"
    fi

    pcs cluster cib ${cibfile}

    while [[ ${1} ]]; do

        # ipaddr=$(grep ^${1} ${HA_CONFIG_FILE} | cut -d = -f 2)
        ipaddrx="VIP_${1//-/_}"

        # echo "${1} ${ipaddrx}"
        ipaddr=${!ipaddrx}

        # echo "pcs -f ${cibfile} resource create ${1}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${ipaddr} cidr_netmask=32 op monitor interval=15s"
        pcs -f ${cibfile} resource create ${1}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${ipaddr} cidr_netmask=32 op monitor interval=15s
        if [ $? -ne 0 ]; then
            echo "warning pcs resource create ${1}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${ipaddr} cidr_netmask=32 op monitor interval=10s failed"
        fi

        # echo "pcs -f ${cibfile} resource create ${1}-trigger_ip-1 ocf:heartbeat:Dummy"
        pcs -f ${cibfile} resource create ${1}-trigger_ip-1 ocf:heartbeat:Dummy
        if [ $? -ne 0 ]; then
            echo "warning: pcs resource create ${1}-trigger_ip-1 ocf:heartbeat:Dummy failed"
        fi

        # echo "pcs -f ${cibfile} constraint colocation add ${1}-cluster_ip-1 with ${1}-trigger_ip-1"
        pcs -f ${cibfile} constraint colocation add ${1}-cluster_ip-1 with ${1}-trigger_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint colocation add ${1}-cluster_ip-1 with ${1}-trigger_ip-1 failed"
        fi

        # echo "pcs -f ${cibfile} constraint order ${1}-trigger_ip-1 then nfs-grace-clone"
        pcs -f ${cibfile} constraint order ${1}-trigger_ip-1 then nfs-grace-clone
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint order ${1}-trigger_ip-1 then nfs-grace-clone failed"
        fi

        # echo "pcs -f ${cibfile} constraint order nfs-grace-clone then ${1}-cluster_ip-1"
        pcs -f ${cibfile} constraint order nfs-grace-clone then ${1}-cluster_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint order nfs-grace-clone then ${1}-cluster_ip-1 failed"
        fi

        shift
    done

    create_virt_ip_constraints ${cibfile} ${HA_SERVERS}

    pcs cluster cib-push ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning pcs cluster cib-push ${cibfile} failed"
    fi
    rm -f ${cibfile}
}

teardown_resources()
{
    # local mntpt=$(grep ha-vol-mnt ${HA_CONFIG_FILE} | cut -d = -f 2)

    # unmount the HA-state volume and terminate ganesha.nfsd on all nodes
    pcs resource create nfs_stop ganesha_nfsd ha_vol_name=dummy ha_vol_mnt=${HA_VOL_MNT} ha_vol_server=dummy --clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource create nfs_stop ganesha_nfsd --clone failed"
    fi
    sleep 1
    # cloned resources seem to never have their start() invoked when they
    # are created, but stop() is invoked when they are destroyed. Why???.
    pcs resource delete nfs_stop-clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource delete nfs_stop-clone failed"
    fi

    while [[ ${1} ]]; do
        echo "pcs resource delete ${1}-cluster_ip-1"
        pcs resource delete ${1}-cluster_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs resource delete ${1}-cluster_ip-1 failed"
        fi
        echo "pcs resource delete ${1}-trigger_ip-1"
        pcs resource delete ${1}-trigger_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs resource delete ${1}-trigger_ip-1 failed"
        fi
        echo "pcs resource delete ${1}-dead_ip-1"
        pcs resource delete ${1}-dead_ip-1
        if [ $? -ne 0 ]; then
            echo "info: pcs resource delete ${1}-dead_ip-1 failed"
        fi
        shift
    done

    # delete -clone resource agents
    echo "pcs resource delete nfs-mon-clone"
    pcs resource delete nfs-mon-clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource delete nfs-mon-clone failed"
    fi

    echo "pcs resource delete nfs-grace-clone"
    pcs resource delete nfs-grace-clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource delete nfs-grace-clone failed"
    fi

}


recreate_resources()
{
    local cibfile=${1}; shift
    local add_node=${1}; shift
    local add_vip=${1}; shift

    while [[ ${1} ]]; do

        # ipaddr=$(grep ^${1} ${HA_CONFIG_FILE} | cut -d = -f 2)
        ipaddrx="VIP_${1//-/_}"

        echo "${1} ${ipaddrx}"
        ipaddr=${!ipaddrx}

        echo "pcs -f ${cibfile} resource create ${1}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${ipaddr} cidr_netmask=32 op monitor interval=15s"
        pcs -f ${cibfile} resource create ${1}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${ipaddr} cidr_netmask=32 op monitor interval=15s
        if [ $? -ne 0 ]; then
            echo "warning pcs resource create ${1}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${ipaddr} cidr_netmask=32 op monitor interval=10s failed"
        fi

        echo "pcs -f ${cibfile} resource create ${1}-trigger_ip-1 ocf:heartbeat:Dummy"
        pcs -f ${cibfile} resource create ${1}-trigger_ip-1 ocf:heartbeat:Dummy
        if [ $? -ne 0 ]; then
            echo "warning: pcs resource create ${1}-trigger_ip-1 ocf:heartbeat:Dummy failed"
        fi

        echo "pcs -f ${cibfile} constraint colocation add ${1}-cluster_ip-1 with ${1}-trigger_ip-1"
        pcs -f ${cibfile} constraint colocation add ${1}-cluster_ip-1 with ${1}-trigger_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint colocation add ${1}-cluster_ip-1 with ${1}-trigger_ip-1 failed"
        fi

        echo "pcs -f ${cibfile} constraint order ${1}-trigger_ip-1 then nfs-grace-clone"
        pcs -f ${cibfile} constraint order ${1}-trigger_ip-1 then nfs-grace-clone
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint order ${1}-trigger_ip-1 then nfs-grace-clone failed"
        fi

        echo "pcs -f ${cibfile} constraint order nfs-grace-clone then ${1}-cluster_ip-1"
        pcs -f ${cibfile} constraint order nfs-grace-clone then ${1}-cluster_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs constraint order nfs-grace-clone then ${1}-cluster_ip-1 failed"
        fi

        shift
    done

    echo "pcs -f ${cibfile} resource create ${add_node}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${add_vip} cidr_netmask=32 op monitor interval=15s"
    pcs -f ${cibfile} resource create ${add_node}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${add_vip} cidr_netmask=32 op monitor interval=15s
    if [ $? -ne 0 ]; then
        echo "warning pcs resource create ${add_node}-cluster_ip-1 ocf:heartbeat:IPaddr ip=${add_vip} cidr_netmask=32 op monitor interval=10s failed"
    fi

    echo "pcs -f ${cibfile} resource create ${add_node}-trigger_ip-1 ocf:heartbeat:Dummy"
    pcs -f ${cibfile} resource create ${add_node}-trigger_ip-1 ocf:heartbeat:Dummy
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource create ${add_node}-trigger_ip-1 ocf:heartbeat:Dummy failed"
    fi

    echo "pcs -f ${cibfile} constraint colocation add ${add_node}-cluster_ip-1 with ${add_node}-trigger_ip-1"
    pcs -f ${cibfile} constraint colocation add ${add_node}-cluster_ip-1 with ${add_node}-trigger_ip-1
    if [ $? -ne 0 ]; then
        echo "warning: pcs constraint colocation add ${add_node}-cluster_ip-1 with ${add_node}-trigger_ip-1 failed"
    fi

    echo "pcs -f ${cibfile} constraint order ${add_node}-trigger_ip-1 then nfs-grace-clone"
    pcs -f ${cibfile} constraint order ${add_node}-trigger_ip-1 then nfs-grace-clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs constraint order ${add_node}-trigger_ip-1 then nfs-grace-clone failed"
    fi

    echo "pcs -f ${cibfile} constraint order nfs-grace-clone then ${add_node}-cluster_ip-1"
    pcs -f ${cibfile} constraint order nfs-grace-clone then ${add_node}-cluster_ip-1
    if [ $? -ne 0 ]; then
        echo "warning: pcs constraint order nfs-grace-clone then ${add_node}-cluster_ip-1 failed"
    fi

}


clear_and_recreate_resources()
{
    local cibfile=${1}; shift
    local add_node=${1}; shift
    local add_vip=${1}; shift

    while [[ ${1} ]]; do

        echo "pcs -f ${cibfile} resource delete ${1}-cluster_ip-1"
        pcs -f ${cibfile} resource delete ${1}-cluster_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs -f ${cibfile} resource delete ${1}-cluster_ip-1"
        fi

        echo "pcs -f ${cibfile} resource delete ${1}-trigger_ip-1"
        pcs -f ${cibfile} resource delete ${1}-trigger_ip-1
        if [ $? -ne 0 ]; then
            echo "warning: pcs -f ${cibfile} resource delete ${1}-trigger_ip-1"
        fi

        shift
    done

    recreate_resources ${cibfile} ${add_node} ${add_vip} ${HA_SERVERS}

}


addnode_create_resources()
{
    local add_node=${1}; shift
    local add_vip=${1}; shift
    local cibfile=$(mktemp -u)

    # mount the HA-state volume and start ganesha.nfsd on the new node
    pcs cluster cib ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning: pcs cluster cib ${cibfile} failed"
    fi

    echo "pcs -f ${cibfile} resource create nfs_start-${add_node} ganesha_nfsd ha_vol_name=${HA_VOL_NAME} ha_vol_mnt=${HA_VOL_MNT} ha_vol_server=${HA_VOL_SERVER}"
    pcs -f ${cibfile} resource create nfs_start-${add_node} ganesha_nfsd ha_vol_name=${HA_VOL_NAME} ha_vol_mnt=${HA_VOL_MNT} ha_vol_server=${HA_VOL_SERVER}
    if [ $? -ne 0 ]; then
        echo "warning: pcs -f ${cibfile} resource create nfs_start-${add_node} ganesha_nfsd ha_vol_name=${HA_VOL_NAME} ha_vol_mnt=${HA_VOL_MNT} ha_vol_server=${HA_VOL_SERVER} failed"
    fi

    pcs -f ${cibfile} constraint location nfs_start-${add_node} prefers ${newnode}=INFINITY
    if [ $? -ne 0 ]; then
        echo "warning: pcs -f ${cibfile} constraint location nfs_start-${add_node} prefers ${newnode}=INFINITY failed"
    fi

    pcs -f ${cibfile} constraint order nfs_start-${add_node} then nfs-mon-clone
    if [ $? -ne 0 ]; then
        echo "warning: pcs -f ${cibfile} constraint order nfs_start-${add_node} then nfs-mon-clone failed"
    fi

    pcs cluster cib-push ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning: pcs cluster cib-push ${cibfile} failed"
    fi

    rm -f ${cibfile}

    # start HA on the new node
    pcs cluster start ${add_node}
    if [ $? -ne 0 ]; then
       echo "warning: pcs cluster start ${add_node} failed"
    fi

    pcs resource delete nfs_start-${add_node}
    if [ $? -ne 0 ]; then
        echo "warning: pcs resource delete nfs_start-${add_node} failed"
    fi


    pcs cluster cib ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning: pcs cluster cib ${cibfile} failed"
    fi

    # delete all the -cluster_ip-1 and -trigger_ip-1 resources,
    # clearing their constraints, then create them again so we can
    # rejigger their constraints
    clear_and_recreate_resources ${cibfile} ${add_node} ${add_vip} ${HA_SERVERS}

    HA_SERVERS="${HA_SERVERS} ${add_node}"

    create_virt_ip_constraints ${cibfile} ${HA_SERVERS}

    pcs cluster cib-push ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning: pcs cluster cib-push ${cibfile} failed"
    fi
}


deletenode_delete_resources()
{
    local node=${1}; shift

    pcs cluster cib ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning: pcs cluster cib ${cibfile} failed"
    fi

    pcs cluster cib-push ${cibfile}
    if [ $? -ne 0 ]; then
        echo "warning: pcs cluster cib-push ${cibfile} failed"
    fi
}

setup_state_volume()
{
    local mnt=$(mktemp -d)
    local longname=""
    local shortname=""
    local dname=""

    mount -t glusterfs ${HA_VOL_SERVER}:/${HA_VOL_NAME} ${mnt}

    longname=$(hostname)
    dname=${longname#$(hostname -s)}

    while [[ ${1} ]]; do
        mkdir ${mnt}/${1}${dname}
        mkdir ${mnt}/${1}${dname}/nfs
        mkdir ${mnt}/${1}${dname}/nfs/ganesha
        mkdir ${mnt}/${1}${dname}/nfs/statd
        touch ${mnt}/${1}${dname}/nfs/state
        mkdir ${mnt}/${1}${dname}/nfs/ganesha/v4recov
        mkdir ${mnt}/${1}${dname}/nfs/ganesha/v4old
        mkdir ${mnt}/${1}${dname}/nfs/statd/sm
        mkdir ${mnt}/${1}${dname}/nfs/statd/sm.bak
        mkdir ${mnt}/${1}${dname}/nfs/statd/state
        echo " 1dname ${1}${dname}"
        for server in ${HA_SERVERS} ; do
            echo "server ${server}"
            echo " 1dname ${1}${dname}"
            if [ ${server} != ${1}${dname} ]; then
                echo "mnt ${mnt}"
                ln -s ${mnt}/${server}/nfs/ganesha ${mnt}/${1}${dname}/nfs/ganesha/${server}
                ln -s ${mnt}/${server}/nfs/statd ${mnt}/${1}${dname}/nfs/statd/${server}
            fi
        done
        shift
    done

    umount ${mnt}
    rmdir ${mnt}
}

main()
{
    local cmd=${1}; shift
    local node=""
    local vip=""

    if [ -e /etc/os-release ]; then
        RHEL6_PCS_CNAME_OPTION=""
    fi

    case "${cmd}" in

    setup | --setup)
        echo "setting up ${HA_NAME}"

        check_cluster_exists ${HA_NAME}

        determine_servers "setup"
        echo "num_nodes = ${HA_NUM_SERVERS}, ${HA_SERVERS}"

        if [ "X${HA_NUM_SERVERS}X" != "X1X" ]; then

            # setup_state_volume ${HA_SERVERS}

            setup_cluster ${HA_NAME} ${HA_NUM_SERVERS} "${HA_SERVERS}"

            echo "creating resources ${HA_SERVERS}"
            setup_create_resources ${HA_SERVERS}

            setup_finalize
        else

            echo "insufficient servers for HA, aborting"
        fi
        ;;

    teardown | --teardown)
        echo "tearing down ${HA_NAME}"

        determine_servers "teardown"
        echo "num_nodes = ${HA_NUM_SERVERS}"

        teardown_resources ${HA_SERVERS}

        teardown_cluster ${HA_NAME}
        ;;

    add | --add)
        node=${1}; shift
        vip=${1}; shift

        echo "adding ${node} with ${vip} to ${HA_NAME}"

        determine_servers "add"

        pcs cluster node add ${node}
        if [ $? -ne 0 ]; then
            echo "warning: pcs cluster node add ${node} failed"
        fi

        addnode_create_resources ${node} ${vip}

        ;;

    delete | --delete)
        node=${1}; shift

        echo "deleting ${node} from ${HA_NAME}"

        determine_servers "delete"

        deletenode_delete_resources ${node}

        pcs cluster node remove ${node}
        if [ $? -ne 0 ]; then
            echo "warning: pcs cluster node remove ${node} failed"
        fi

        ;;

    status | --status)
        ;;


    *)
        echo "Usage: ganesha-ha.sh setup|teardown|add|delete|status"
        ;;

    esac
}

main $*


#!bin/sh
case $1 in
      start)
            echo "loading modules"
            /lib/modules/6.1.44/scull_load
            if [ $? -ne 0 ]; then
                echo "Failed to load scull module"
                exit 1
            fi 

            /lib/modules/6.1.44/module_load /lib/modules/6.1.44/misc-modules.ko
            if [ $? -ne 0 ]; then
                echo "Failed to load hello module"
                exit 1
            fi        
            ;;
      stop)
           echo "unloading modules"

           /lib/modules/6.1.44/module_unload misc-modules
           if [ $? -ne 0 ]; then
               echo "Failed to unload hello module"
               exit 1
           fi

           /lib/modules/6.1.44/scull_unload
           if [ $? -ne 0 ]; then
               echo "Failed to unload scull module"
               exit 1
           fi
           ;;
      *)
            echo "Usage: $0 {start|stop}"
            exit 1
esac
exit 0
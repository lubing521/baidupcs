C/C++写的一个百度网盘工具，可以在linux终端中使用。 <br/>
这是通过分析网盘网站得到的直接接口，不需要创建应用。<br/>
支持AES-CBC-128, AES-CBC-192, AES-CBC-256加密。<br/>

编译安装：
===================================
程序依赖于 libcurl。

### 1. 安装依赖
    apt-get install libcurl4-openssl-dev
### 2. 获取源代码
    git clone https://github.com/GangZhuo/baidupcs.git
### 3. 编译源代码
    cd baidupcs
    make clean
    make
    make install #将安装到/usr/local/bin下
### 4. 手动安装到其他目录，例如 /usr/bin 下 
    cp ./bin/pcs /usr/bin/
    chmod a+x /usr/bin/pcs (此行命令非必须执行)

命令：
===================================
如果出现中文乱码，请检查操作系统的当前编码是否为UTF8。<br/>
命令中涉及到的网盘文件路径，可以是绝对路径，也可以是相对于当前工作目录的相对路径。<br/>
例：<br/>
    /data/data.txt 即是网盘绝对路径，表示网盘根目录下的data目录中的data.txt文件。<br/>
    data.txt 即是相对路径，表示当前工作目录下的data.txt文件。<br/>
    当前工作目录可通过 'pcs pwd' 命令来查看，当前工作目录可通过 'pcs cd'命令切换。<br/>
    可通过 'pcs help' 查看支持的子命令；可在子命令后加 '-h' 查看子命令帮助，<br/>
    也可调用类似于 'pcs help <command>' 的命令来查看子命令的帮助。<br/>
    例如：<br/>
        pcs move -h。<br/>
        pcs help move 此命令等价于上一行的命令<br/>


### 直接显示网盘中文本文件内容
    pcs cat <file>
    
    示例：
      pcs cat note.txt
      pcs cat /my/todo.txt

### 切换工作目录
    pcs cd <dir>
    
    示例：
      pcs cd pcs
      pcs cd /backup/20140618

### 复制网盘文件或目录
    pcs copy <old path> <new path>
    
    示例：
      pcs copy data.txt data_20140118.txt

### 比较异同
    pcs compare [-cderu] <local path> <remote path>
    
    比较本地文件和远端文件、本地目录和远端目录的异同。
    默认选项是'-cdu'，即打印出需要上传的文件、
    需要下载的文件和无法确定上传下载的文件。
    
    比较规则：
      只通过时间来进行比较。
      I)  如果本地和远端都是文件：
            a) 本地最后修改时间大于网盘文件上传时间，则认为需要上传；
            b) 本地最后修改时间小于网盘文件上传时间，则认为需要下载；
               （此处注意：当一个文件上传到网盘后，其网盘时间肯定比本地最后修改时间大。
                 如果此时执行比较的话，则会认为该文件需要从网盘下载。
                 下载则没有此问题，因为一个文件下载后，程序会使用
                 网盘时间来更新本地文件的最后修改时间。）
      II)  本地存在，网盘不存在，则认为需要上传
      III) 本地不存在，网盘存在，则认为需要下载
      IV)  如果一端是文件，另一端是目录，则认为无法确认是上传还是下载。
    
    选项：
      -c  只打印出无法确定上传或下载的项。
          一般是因为本地是文件，远端是目录，或本地是目录远端是文件
      -d  只打印出需要下载的文件或目录
      -e  只打印相同的文件或目录
      -r  递归比较其子目录
      -u  只打印出需要上传的文件或目录
    
    示例：
       pcs compare music music
       pcs compare -r ~/music music
    

### 显示当前上下文
    pcs context
    
    上下文包括：
        当前使用的Cookie文件、验证码图片保存路径、
        当前的工作目录、列出目录时分页大小、列出目录时的排序字段、
        列出目录时的排序方向、启用的加密方法、加密密钥、是否启用加密
    
    程序开启时会自动读取上下文文件；结束时，会自动保存上下文到文件。
    可通过'PCS_CONTEXT'环境变量指定上下文文件路径。程序判断规则是：
       1) 如果通过'--context'选项指定了上下文文件，则使用它；
       2) 如果未通过'--context'指定，但是指定了环境变量'PCS_CONTEXT'则使用它；
       3) 如果也未指定环境变量'PCS_CONTEXT'则使用'~/.pcs/pcs.context'

    上下文为JSON格式的文件：
    
    {
	    "cookiefile":	        "/home/gang/.pcs/default.cookie", /*指定Cookie文件*/
	    "captchafile":	        "/home/gang/.pcs/captcha.gif",    /*指定验证码图片保存路径*/
	    "workdir":	            "/",                              /*指定当期工作目录*/
	    "list_page_size":	    20,                               /*指定列出目录时分页大小*/
	    "list_sort_name":	    "name",                           /*指定列出目录时排序字段*/
	    "list_sort_direction":	"asc",                            /*指定列出目录时的排序方向*/
	    "secure_method":	    "aes-cbc-128",                    /*指定上传时的加密方式*/
	    "secure_key":	        "12345678",                       /*指定上传时的加密密钥，*/
	                                                              /*下载时如检测到文件被加密，也使用此密钥解密*/
	    "secure_enable":	    true                              /*指定是否启用加密解密*/
	                                                              /*如果设置为false，*/
	                                                              /*下载时即使检查到文件加密，也不会解密*/
    }
    

### 下载文件
    pcs download [-f] <remote file> <local file>
    
    只能下载文件，如果需要下载目录，请使用 'pcs synch -d <remote dir> <local dir>'。
    
    选项：
      -f   如果本地文件存在的话，强制替换
    
    示例：
      pcs download /backup/data.20140118.tar.gz ~/download/data.20140118.tar.gz

### 直接保存文本到网盘中
    pcs echo [-a] <remote file> <text>
    
    选项：
      -a  指定把文本添加到文件末尾，而不是完全替换
      
    示例：
      pcs echo data.txt "The text that saved by pcs."

### 查看帮助
    pcs help [command name]
    
    查看帮助。
    
    示例：
       pcs help
       pcs help compare
       pcs help -h

### 列出网盘根目录下的文件或目录
    pcs list [dir]

    输出格式为：
        * 第一列指示是否是目录，如果是目录则输出 d，否则输出 -
        * 第二列是文件或目录的最后修改时间
        * 第三列是文件路径

    示例：
       pcs list
       pcs list /music
       pcs list -h
       
    列出目录时会自动分页显示，如果需要修改分页大小的话，
    使用'pcs set --list_page_size=20'来修改，把list_page_size设置为0，则关闭分页。

### 登录网盘
    pcs login [--username=<username>] [--password=<password>]
    
    登录可能需要输入验证码。目前的处理办法是把验证码图片写入到本地文件，用户打开文件识别验证码。
    可通过 'pcs set --cookie_file=<path> 来指定验证码保存路径，
    例如：'pcs set --cookie_file=/var/www/captch.git'
    可通过 'pcs context' 查看当前的执行上下文。执行上下文包括验证码图片保存路径等其他信息。
    
    示例：
      pcs login -h 可查看login命令的使用方法
      pcs login    会提示输入用户名和密码
      pcs login --username=gang 指定用户名登录
      pcs login --username=gang --password=123456 指定用户名和密码登录
      
### 退出网盘
    pcs logout

### 显示网盘文件或目录的元数据
    pcs meta <file>
    
    示例：
       pcs meta
       pcs meta note.txt

### 创建目录
    pcs mkdir <dir>
    
    示例：
      pcs mkdir subdir
      pcs mkdir /music/china

### 移动网盘文件或目录
    pcs move <src> <dst>
    
    示例：
      pcs move /data_20140118.txt /subdir/data.txt
      pcs move music /my/music

### 显示当前网盘的工作目录
    pcs pwd

    示例：
      pcs pwd

### 显示网盘配额
    pcs quota [-e]
    
    选项：
      -e   打印精确的网盘配额
      
    示例：
       pcs quota
       pcs quota -e

### 删除文件或目录
    pcs remove <path>
    
    示例：
      pcs remove /subdir/data_20140118.txt

### 重命名网盘文件或目录
    pcs rename <src> <new name>
    
    注意：<new name>是新的文件名字，而不是文件路径。如果需要移动文件到另一个目录，请使用 'pcs move'。
    
    示例：
      pcs rename /data.txt data_20140118.txt

### 设置上下文
    pcs set [--captcha_file=<path>] [--cookie_file=<path>] ...
    
    选项：
	--captcha_file=<file path>         设置验证码图片保存路径
	--cookie_file=<file path>          设置cookie文件路径
	--list_page_size=<page size>       设置列出目录时分页大小
	--list_sort_direction=[asc|desc]   设置列出目录时排序方向
	--list_sort_name=[name|time|size]  设置列出目录时排序字段
	--secure_enable=[true|false]       设置上传下载时是否启用加密
	--secure_key=<key string>          设置加密解密密钥
	--secure_method=[plaintext|aes-cbc-128|aes-cbc-192|aes-cbc-256] 设置加密方式

    示例：
      pcs set -h
      pcs set --cookie_file="/tmp/pcs.cookie"
      pcs set --captcha_file="/tmp/vc.git"
      pcs set --list_page_size=20 --list_sort_name=name --list_sort_direction=desc
      pcs set --secure_enable=true --secure_key=123456 --secure_method=aes-cbc-256

### 搜索文件
    pcs search [-r] [dir] <key>
    
    示例：
       pcs search note.txt          在当前工作目录搜索 note.txt
       pcs search /music desc.mp3   在/music目录搜索 desc.mp3
       pcs search -r note.txt       在当前工作目录递归搜索 note.txt
       pcs search -r /music desc.mp3 在/music目录递归搜索 desc.mp3

### 同步目录
    pcs synch [-cdenru] <local path> <remote path>
    
    同步本地文件和远端文件、本地目录和远端目录。
    默认选项是'-cdu'，即上传需要上传的文件、下载需要下载的文件和打印无法确定上传下载的文件。
    
    比较规则：（同'compare'一样）
      只通过时间来进行比较。
      I)  如果本地和远端都是文件：
            a) 本地最后修改时间大于网盘文件上传时间，则认为需要上传；
            b) 本地最后修改时间小于网盘文件上传时间，则认为需要下载；
               （此处注意：当一个文件上传到网盘后，其网盘时间肯定比本地最后修改时间大。
                 如果此时执行比较的话，则会认为该文件需要从网盘下载。
                 下载则没有此问题，因为一个文件下载后，程序会使用
                 网盘时间来更新本地文件的最后修改时间。）
      II)  本地存在，网盘不存在，则认为需要上传
      III) 本地不存在，网盘存在，则认为需要下载
      IV)  如果一端是文件，另一端是目录，则认为无法确认是上传还是下载。
    
    选项：
      -c  只打印出无法确定上传或下载的项。
          一般是因为本地是文件，远端是目录，或本地是目录远端是文件
      -d  只下载需要下载的文件或目录
      -e  只打印相同的文件或目录
      -n  只是打印，而不真正的执行上传和下载，等价于'compare'
      -r  递归比较其子目录
      -u  只上传需要上传的文件或目录
    
    示例：
       pcs synch music music
       pcs synch -r ~/music music
       pcs synch -u music music     只上传需要上传的文件，等价于备份
       pcs synch -d music music     只下载需要下载的文件，等价于还原
       pcs synch -du music music    上传需要上传的文件，并且下载需要下载的文件，等价于同步
       
    注意：推荐每次都带上'-c'选项，可以打印出不知道如何处理的文件或目录，防止漏上传或下载。

### 上传文件
    pcs upload [-f] <local file> <remote file>
    
    只能上传文件，如果需要上传目录，请使用 'pcs synch' 命令。
    
    示例：
       pcs upload ~/data.tar.gz /backup/data.20140118.tar.gz

### 显示程序版本
    pcs version
    
    示例：
       pcs version

### 显示当前登录用户
    pcs who

    示例：
       pcs who


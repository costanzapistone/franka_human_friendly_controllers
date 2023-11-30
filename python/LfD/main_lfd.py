# %%
from Learning_from_demonstration import LfD
import rospy
#%%
LfD=LfD()
LfD.home_gripper() # homeing the gripper allows to kinestheicall move it. 
rospy.sleep(5)


#%%
LfD.save_as_home()
#%%
LfD.load_home_pose()
#%%
LfD.go_to_home()
#%%
LfD.traj_rec()

#%%
# LfD.traj_rec_keyboard()
#%%
LfD.save()
#%%
LfD.save(file="left")
#%%
LfD.save(file="right")
# Execute left trajectory
#%%
LfD.load(file="left")
#%%
LfD.execute()
# Execute left trajectory
#%%
LfD.load(file="right")
#%%
LfD.execute()
# %%

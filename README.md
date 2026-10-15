##Systéme de reconaissance faciale.

Ce projet est un projet de groupe réalisé dans le cadre scolaire. Ma partie consiste à faire réaliser le code qui gére la prise de photo par l'esp32 CAM et le code qui gére l'esp8266.

##Fonctionnalités du projet:
-Capture d'image avec la camera de l'esp32 CAM.
-Dectection et reconnaissance des visages enregistrés en BD.
-Autorisation ou refus de l'accés.

##Materiel et logiciels utilisé:
-Une esp32 CAM
-Une esp8266
-Un serveur python
-FireBase
-Arduino IDE

##Mode de fonctionnement:
Une fois la photo prise sur l'interface de la camera, elle est comparée grace à un programme python au photos qui sont dans la base de données, s'il y a correspondance, le portail s'ouvre et laisse la personne passer sinon, l'accées est refusé. 

##Limites: 
La qualité d'image n'est pas trés bonne ainsi, la reconnaissance facial ne fonctionne pas toujours. 

